#include "video_item.hpp"

#include "msc/video/frame.hpp"
#include "msc/video/letterbox.hpp"

#include <rhi/qrhi.h>
#include <QFile>
#include <QMatrix4x4>
#include <QQuickWindow>
#include <QVector3D>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <utility>

namespace {

using msc::video::Frame;
using msc::video::FrameSource;
using namespace std::chrono_literals;

// How often the renderer turns its timings into statistics for the GUI.
constexpr auto kStatsInterval = 250ms;
// How often the GUI thread checks that the picture is live (REQ-VID-003). A frozen picture is
// flagged at most this long after it passes msc::video::kFrozenAfter. The check is one atomic
// load and a subtraction.
constexpr auto kWatchdogPeriod = 50ms;

// Bars around the picture. Black, as on a real endoscope monitor: anything brighter pulls
// the eye away from the image.
const QColor kBarColour = Qt::black;

// Qt sizes are int; letterbox() works in unsigned pixels. Sizes are never negative.
std::uint32_t pixels(int length) {
    return static_cast<std::uint32_t>(std::max(length, 0));
}

// The overlay shader's uniform block (std140): a mat4, then two vec4.
struct HudUniforms {
    std::array<float, 16> transform{};
    std::array<float, 4> colour{};
    std::array<float, 4> ring{};  // inner radius, outer radius, edge softness, unused
};
static_assert(sizeof(HudUniforms) == 96, "must match the Hud block in hud.vert / hud.frag");

QShader loadShader(const QString& resourcePath) {
    QFile file(resourcePath);
    if (!file.open(QIODevice::ReadOnly)) {
        qWarning("VideoItem: cannot open shader %s", qPrintable(resourcePath));
        return {};
    }
    return QShader::fromSerialized(file.readAll());
}

}  // namespace

// Outside the anonymous namespace so VideoItem can name it as a friend.
class VideoRenderer final : public QQuickRhiItemRenderer {
protected:
    void initialize(QRhiCommandBuffer* cb) override;
    void synchronize(QQuickRhiItem* item) override;
    void render(QRhiCommandBuffer* cb) override;

private:
    void createPipeline(QRhiCommandBuffer* cb);
    // Makes the texture match the frame size. Returns false if it could not be created.
    bool ensureTextureSize(const QSize& size);
    void uploadFrame(const Frame& frame, QRhiResourceUpdateBatch* updates);
    void createHudPipeline(QRhiCommandBuffer* cb);
    // The marker for the frame on screen, and its uniforms into `updates` if it is to be drawn.
    [[nodiscard]] bool prepareHud(const msc::video::PixelRect& box, QRhiResourceUpdateBatch*& updates);
    // Render thread, right after the window's swap (Present) has returned.
    void onSwapped();

    std::shared_ptr<FrameSource> source_;
    const msc::Clock* clock_ = nullptr;
    bool hasFrame_ = false;  // a frame has been uploaded since the source was set

    // The overlay (M5.3). The frame in the texture, by number and capture time: the marker is
    // looked up for exactly this frame, also on draws that bring no new frame.
    std::shared_ptr<msc::hud::PoseFeed> poses_;
    msc::hud::HudOverlay overlay_;
    std::uint64_t shownSequence_ = 0;
    msc::Nanoseconds shownCapture_{0};

    // REQ-VID-002: timings along the path, and where the statistics go.
    msc::video::PresentationTracker tracker_;
    std::shared_ptr<VideoStatsChannel> channel_;
    msc::Nanoseconds lastStatsAt_{0};
    // Receives QQuickWindow::frameSwapped. Created on the render thread, so the direct
    // connection runs there; deleting it with the renderer disconnects it.
    QQuickWindow* window_ = nullptr;
    std::unique_ptr<QObject> swapListener_;

    QRhi* rhi_ = nullptr;
    int sampleCount_ = 0;
    QRhiTexture::Format targetFormat_ = QRhiTexture::UnknownFormat;
    std::unique_ptr<QRhiBuffer> vertices_;
    std::unique_ptr<QRhiTexture> texture_;
    std::unique_ptr<QRhiSampler> sampler_;
    std::unique_ptr<QRhiShaderResourceBindings> bindings_;
    std::unique_ptr<QRhiGraphicsPipeline> pipeline_;
    std::unique_ptr<QRhiBuffer> hudVertices_;
    std::unique_ptr<QRhiBuffer> hudUniforms_;
    std::unique_ptr<QRhiShaderResourceBindings> hudBindings_;
    std::unique_ptr<QRhiGraphicsPipeline> hudPipeline_;
};

// Called on the render thread whenever the item's render target is (re)created: first show,
// resize, sample count change. Builds the pipeline only when something it depends on changed.
void VideoRenderer::initialize(QRhiCommandBuffer* cb) {
    if (rhi_ != rhi()) {
        rhi_ = rhi();
        pipeline_.reset();
    }
    if (sampleCount_ != renderTarget()->sampleCount()) {
        sampleCount_ = renderTarget()->sampleCount();
        pipeline_.reset();
    }
    const QRhiTexture* finalTexture = sampleCount_ > 1 ? resolveTexture() : colorTexture();
    if (targetFormat_ != finalTexture->format()) {
        targetFormat_ = finalTexture->format();
        pipeline_.reset();
    }
    if (!pipeline_) {
        createPipeline(cb);
        createHudPipeline(cb);
    }
}

void VideoRenderer::createPipeline(QRhiCommandBuffer* cb) {
    // A triangle strip over the whole viewport: x, y, u, v per corner. Written in the
    // OpenGL convention (y up), with v = 0 at the top row of the image, which is the first
    // row in Frame::pixels and the first row QRhi uploads on every backend.
    constexpr std::size_t kFloatsPerVertex = 4;
    std::array<float, 4 * kFloatsPerVertex> quad{
        -1.0F, -1.0F, 0.0F, 1.0F,  // bottom left
        1.0F,  -1.0F, 1.0F, 1.0F,  // bottom right
        -1.0F, 1.0F,  0.0F, 0.0F,  // top left
        1.0F,  1.0F,  1.0F, 0.0F,  // top right
    };
    // Backends disagree on which way clip-space y points (Vulkan: down). QRhi gives the
    // matrix that maps the OpenGL convention onto this backend's; the quad never moves, so it
    // is applied here once instead of in the vertex shader every frame.
    const QMatrix4x4 correction = rhi_->clipSpaceCorrMatrix();
    for (std::size_t i = 0; i < quad.size(); i += kFloatsPerVertex) {
        const QVector3D p = correction.map(QVector3D(quad[i], quad[i + 1], 0.0F));
        quad[i] = p.x();
        quad[i + 1] = p.y();
    }

    vertices_.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(quad)));
    vertices_->create();
    QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();
    updates->uploadStaticBuffer(vertices_.get(), quad.data());
    cb->resourceUpdate(updates);

    // Placeholder until the first frame tells the real size; never drawn (hasFrame_ is false).
    texture_.reset(rhi_->newTexture(QRhiTexture::RGBA8, QSize(16, 16)));
    texture_->create();

    // Linear filtering: the image is scaled to the window, nearest would make it blocky.
    sampler_.reset(rhi_->newSampler(QRhiSampler::Linear, QRhiSampler::Linear, QRhiSampler::None,
                                    QRhiSampler::ClampToEdge, QRhiSampler::ClampToEdge));
    sampler_->create();

    bindings_.reset(rhi_->newShaderResourceBindings());
    bindings_->setBindings({QRhiShaderResourceBinding::sampledTexture(
        0, QRhiShaderResourceBinding::FragmentStage, texture_.get(), sampler_.get())});
    bindings_->create();

    pipeline_.reset(rhi_->newGraphicsPipeline());
    pipeline_->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline_->setSampleCount(sampleCount_);
    pipeline_->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/video.vert.qsb"))},
        {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/video.frag.qsb"))},
    });
    QRhiVertexInputLayout layout;
    layout.setBindings({{kFloatsPerVertex * sizeof(float)}});
    layout.setAttributes({
        {0, 0, QRhiVertexInputAttribute::Float2, 0},
        {0, 1, QRhiVertexInputAttribute::Float2, 2 * sizeof(float)},
    });
    pipeline_->setVertexInputLayout(layout);
    pipeline_->setShaderResourceBindings(bindings_.get());
    pipeline_->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline_->create()) {
        qWarning("VideoItem: could not create the graphics pipeline; video will not be shown");
        pipeline_.reset();
    }
}

// The overlay's own small pipeline: a quad around the marker, a ring drawn by the fragment
// shader, blended over the picture.
void VideoRenderer::createHudPipeline(QRhiCommandBuffer* cb) {
    hudPipeline_.reset();
    // Corners of the quad, -1 to 1. Not corrected for the backend here: the marker moves every
    // frame, so its matrix (which includes the correction) is a uniform.
    constexpr std::array<float, 8> corners{-1.0F, -1.0F, 1.0F, -1.0F, -1.0F, 1.0F, 1.0F, 1.0F};
    hudVertices_.reset(rhi_->newBuffer(QRhiBuffer::Immutable, QRhiBuffer::VertexBuffer, sizeof(corners)));
    hudVertices_->create();
    QRhiResourceUpdateBatch* updates = rhi_->nextResourceUpdateBatch();
    updates->uploadStaticBuffer(hudVertices_.get(), corners.data());
    cb->resourceUpdate(updates);

    hudUniforms_.reset(rhi_->newBuffer(QRhiBuffer::Dynamic, QRhiBuffer::UniformBuffer, sizeof(HudUniforms)));
    hudUniforms_->create();
    hudBindings_.reset(rhi_->newShaderResourceBindings());
    hudBindings_->setBindings({QRhiShaderResourceBinding::uniformBuffer(
        0, QRhiShaderResourceBinding::VertexStage | QRhiShaderResourceBinding::FragmentStage,
        hudUniforms_.get())});
    hudBindings_->create();

    auto pipeline = std::unique_ptr<QRhiGraphicsPipeline>(rhi_->newGraphicsPipeline());
    pipeline->setTopology(QRhiGraphicsPipeline::TriangleStrip);
    pipeline->setSampleCount(sampleCount_);
    pipeline->setShaderStages({
        {QRhiShaderStage::Vertex, loadShader(QStringLiteral(":/shaders/hud.vert.qsb"))},
        {QRhiShaderStage::Fragment, loadShader(QStringLiteral(":/shaders/hud.frag.qsb"))},
    });
    // Ordinary "over" blending for the colour. The alpha written stays 1: the item is opaque,
    // and Qt Quick composes it into the window by that alpha.
    QRhiGraphicsPipeline::TargetBlend blend;
    blend.enable = true;
    blend.srcColor = QRhiGraphicsPipeline::SrcAlpha;
    blend.dstColor = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    blend.srcAlpha = QRhiGraphicsPipeline::One;
    blend.dstAlpha = QRhiGraphicsPipeline::OneMinusSrcAlpha;
    pipeline->setTargetBlends({blend});
    QRhiVertexInputLayout layout;
    layout.setBindings({{2 * sizeof(float)}});
    layout.setAttributes({{0, 0, QRhiVertexInputAttribute::Float2, 0}});
    pipeline->setVertexInputLayout(layout);
    pipeline->setShaderResourceBindings(hudBindings_.get());
    pipeline->setRenderPassDescriptor(renderTarget()->renderPassDescriptor());
    if (!pipeline->create()) {
        qWarning("VideoItem: could not create the overlay's pipeline; the video is shown without it");
        return;
    }
    hudPipeline_ = std::move(pipeline);
}

// GUI thread blocked, render thread running: the only moment both sides may be touched.
void VideoRenderer::synchronize(QQuickRhiItem* item) {
    auto* video = static_cast<VideoItem*>(item);
    if (video->source_ != source_ || video->clock_ != clock_) {
        // If this drops the last reference, the old source stops its thread here, on the
        // render thread. That is fine: stop() only joins a thread that is already finishing.
        source_ = video->source_;
        clock_ = video->clock_;
        hasFrame_ = false;
        tracker_.reset();
        overlay_.forgetFrame();  // the new source's frame numbers start again
        video->channel_->newestCapture.store(VideoStatsChannel::kNoFrame, std::memory_order_relaxed);
    }
    poses_ = video->poses_;
    channel_ = video->channel_;

    // frameSwapped is emitted on the render thread after the swap: the "present" timestamp.
    if (video->window() != window_) {
        window_ = video->window();
        swapListener_ = std::make_unique<QObject>();
        if (window_ != nullptr) {
            QObject::connect(
                window_, &QQuickWindow::frameSwapped, swapListener_.get(), [this] { onSwapped(); },
                Qt::DirectConnection);
        }
    }
}

void VideoRenderer::onSwapped() {
    if (clock_ == nullptr) {
        return;
    }
    if (channel_ && channel_->resetRequested.exchange(false, std::memory_order_relaxed)) {
        tracker_.reset();
    }
    const msc::Nanoseconds now = clock_->now();
    tracker_.swapped(now);
    if (channel_ && now - lastStatsAt_ >= kStatsInterval) {
        channel_->stats.publish(tracker_.takeStats(now));
        lastStatsAt_ = now;
    }
}

bool VideoRenderer::ensureTextureSize(const QSize& size) {
    if (texture_->pixelSize() == size) {
        return true;
    }
    // Only when the source changes format (not per frame): reallocates GPU memory.
    texture_->setPixelSize(size);
    if (!texture_->create()) {
        qWarning("VideoItem: could not create a %dx%d texture", size.width(), size.height());
        return false;
    }
    // The bindings point at the native texture that was just replaced.
    bindings_->create();
    return true;
}

void VideoRenderer::uploadFrame(const Frame& frame, QRhiResourceUpdateBatch* updates) {
    // No copy on our side: fromRawData wraps the frame's own pixels. The backend may read
    // them as late as the end of this Qt Quick frame (D3D11 and OpenGL defer the upload to
    // endFrame). That is safe because the frame is ours until the next acquireLatest(), and
    // that only happens in the next render() call, after this frame has ended (ADR 0003).
    const QByteArray pixels = QByteArray::fromRawData(reinterpret_cast<const char*>(frame.pixels.data()),
                                                      static_cast<qsizetype>(frame.pixels.size()));
    QRhiTextureSubresourceUploadDescription image(pixels);
    image.setDataStride(static_cast<quint32>(frame.strideBytes()));
    updates->uploadTexture(texture_.get(), QRhiTextureUploadDescription({0, 0, image}));
}

// Decides whether the frame on screen gets a marker (REQ-HUD-001: only if the arm's pose at the
// frame's capture time is known) and, if so, where: the same projection the camera used, into
// the same letterbox rectangle `box` the picture is drawn in.
bool VideoRenderer::prepareHud(const msc::video::PixelRect& box, QRhiResourceUpdateBatch*& updates) {
    using msc::hud::HudMode;
    using msc::hud::HudState;
    const HudMode mode = channel_ ? channel_->hudMode.load(std::memory_order_relaxed) : HudMode::Off;
    msc::hud::HudMarker marker;
    if (mode == HudMode::Off) {
        marker.state = HudState::Off;
    } else if (!hasFrame_ || clock_ == nullptr || box.width == 0 || box.height == 0) {
        marker.state = HudState::Stale;  // no picture: nothing to put a marker on
    } else {
        const double aspect = static_cast<double>(texture_->pixelSize().width()) /
                              static_cast<double>(texture_->pixelSize().height());
        marker = overlay_.markerFor(shownSequence_, shownCapture_, clock_->now(), aspect, mode);
    }
    if (channel_) {
        channel_->hudState.store(marker.state, std::memory_order_relaxed);
    }
    if (!marker.shown() || !hudPipeline_) {
        return false;
    }

    // The quad: the ring plus one pixel of soft edge, as a fraction of the picture's width.
    const double pixel = 1.0 / static_cast<double>(box.width);
    const double outer = marker.radius + (marker.thickness / 2.0);
    const double inner = marker.radius - (marker.thickness / 2.0);
    const double half = outer + (2.0 * pixel);
    // The viewport is the picture, so the picture's corners are at -1 and 1 (y up, as the
    // video quad is written). Widths are fractions of the picture's width, which is 2 units
    // across; the same length is `aspect` times as many units of the height.
    const double boxAspect = static_cast<double>(box.width) / static_cast<double>(box.height);
    QMatrix4x4 transform = rhi_->clipSpaceCorrMatrix();
    transform.translate(static_cast<float>((2.0 * marker.centre.x) - 1.0),
                        static_cast<float>(1.0 - (2.0 * marker.centre.y)));
    transform.scale(static_cast<float>(2.0 * half), static_cast<float>(2.0 * half * boxAspect));

    const auto& rgb = mode == HudMode::Naive ? msc::hud::kHudNaiveColour : msc::hud::kHudSyncedColour;
    HudUniforms uniforms{};
    std::copy_n(transform.constData(), 16, uniforms.transform.begin());
    uniforms.colour = {static_cast<float>(rgb[0]) / 255.0F, static_cast<float>(rgb[1]) / 255.0F,
                       static_cast<float>(rgb[2]) / 255.0F, 1.0F};
    uniforms.ring = {static_cast<float>(inner / half), static_cast<float>(outer / half),
                     static_cast<float>(pixel / half), 0.0F};
    if (updates == nullptr) {
        updates = rhi_->nextResourceUpdateBatch();
    }
    updates->updateDynamicBuffer(hudUniforms_.get(), 0, sizeof(uniforms), &uniforms);
    return true;
}

void VideoRenderer::render(QRhiCommandBuffer* cb) {
    // Every pose the arm link has sent since the last draw, in order, into the history.
    if (poses_) {
        msc::hud::TimedPose pose;
        while (poses_->next(pose)) {
            overlay_.addPose(pose);
        }
    }

    QRhiResourceUpdateBatch* updates = nullptr;
    // Exactly one acquire per rendered frame (ADR 0002): the frame stays valid until the next.
    if (pipeline_ && source_ && source_->acquireLatest()) {
        const Frame& frame = source_->latest();
        const QSize size(static_cast<int>(frame.width), static_cast<int>(frame.height));
        if (frame.format == msc::video::PixelFormat::Rgba8 && !frame.empty() && ensureTextureSize(size)) {
            updates = rhi_->nextResourceUpdateBatch();
            uploadFrame(frame, updates);
            hasFrame_ = true;
            shownSequence_ = frame.sequence;
            shownCapture_ = frame.captureTime;
            if (clock_ != nullptr) {
                tracker_.frameUploaded(frame.sequence, frame.captureTime, clock_->now());
            }
            // Drawn in this frame, presented at its swap a few milliseconds from now. Relaxed:
            // a single number, nothing else is read through it.
            if (channel_) {
                channel_->newestCapture.store(frame.captureTime.count(), std::memory_order_relaxed);
            }
        }
    }

    // Where the picture goes in the item, and the overlay with it: one rectangle for both.
    msc::video::PixelRect box;
    if (hasFrame_) {
        const QSize target = renderTarget()->pixelSize();
        const QSize image = texture_->pixelSize();
        box = msc::video::letterbox(pixels(image.width()), pixels(image.height()), pixels(target.width()),
                                    pixels(target.height()));
    }
    const bool drawHud = prepareHud(box, updates);

    cb->beginPass(renderTarget(), kBarColour, {1.0F, 0}, updates);
    if (hasFrame_) {
        const QSize target = renderTarget()->pixelSize();
        // QRhiViewport counts y from the bottom (OpenGL style); PixelRect counts from the top.
        const std::uint32_t fromBottom = pixels(target.height()) - box.y - box.height;
        cb->setGraphicsPipeline(pipeline_.get());
        cb->setViewport(QRhiViewport(static_cast<float>(box.x), static_cast<float>(fromBottom),
                                     static_cast<float>(box.width), static_cast<float>(box.height)));
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput input(vertices_.get(), 0);
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);

        // The marker, in the same pass and the same viewport as the frame it was computed for:
        // the two reach the screen together or not at all.
        if (drawHud) {
            cb->setGraphicsPipeline(hudPipeline_.get());
            cb->setShaderResources();
            const QRhiCommandBuffer::VertexInput hudInput(hudVertices_.get(), 0);
            cb->setVertexInput(0, 1, &hudInput);
            cb->draw(4);
        }
    }
    cb->endPass();

    if (source_) {
        update();  // render again next frame: there will be a new picture
    }
}

VideoItem::VideoItem(QQuickItem* parent) : QQuickRhiItem(parent) {
    // The renderer publishes statistics every kStatsInterval; pick them up at the same pace.
    statsTimer_.setInterval(kStatsInterval);
    connect(&statsTimer_, &QTimer::timeout, this, [this] {
        if (channel_->stats.read(stats_)) {
            emit statsChanged();
        }
    });
    statsTimer_.start();

    watchdogTimer_.setInterval(kWatchdogPeriod);
    connect(&watchdogTimer_, &QTimer::timeout, this, &VideoItem::checkVideoSignal);
    watchdogTimer_.start();
}

// REQ-VID-003. Runs on the GUI thread, on its own timer: it must not depend on the renderer
// or the source being healthy, since they are what it watches. (If the GUI thread itself
// hangs, nothing on screen changes at all; that needs a watchdog outside the process.)
void VideoItem::checkVideoSignal() {
    std::optional<msc::Nanoseconds> newest;
    const std::int64_t captured = channel_->newestCapture.load(std::memory_order_relaxed);
    if (source_ && clock_ != nullptr && captured != VideoStatsChannel::kNoFrame) {
        newest = msc::Nanoseconds{captured};
    }
    const msc::Nanoseconds now = clock_ != nullptr ? clock_->now() : msc::Nanoseconds{0};
    const auto state = static_cast<VideoState>(msc::video::classifySignal(newest, now));
    const double age = newest ? std::chrono::duration<double, std::milli>(now - *newest).count() : 0.0;

    if (state != videoState_) {
        videoState_ = state;
        emit videoStateChanged();
    }
    // The overlay's state for the frame on screen, on the same timer.
    if (const msc::hud::HudState hud = channel_->hudState.load(std::memory_order_relaxed); hud != hudState_) {
        hudState_ = hud;
        emit hudChanged();
    }
    // Only the frozen banner shows the age; no need to wake QML 20 times a second otherwise.
    if (state != VideoState::Live || frameAgeMs_ != 0.0) {
        frameAgeMs_ = state == VideoState::Live ? 0.0 : age;
        emit frameAgeChanged();
    }
}

void VideoItem::setSourceStatus(const QString& status) {
    if (status == sourceStatus_) {
        return;
    }
    sourceStatus_ = status;
    emit sourceStatusChanged();
}

void VideoItem::setPoseFeed(std::shared_ptr<msc::hud::PoseFeed> poses) {
    poses_ = std::move(poses);
    update();  // synchronize() hands it to the renderer
}

void VideoItem::setHudMode(HudMode mode) {
    const auto wanted = static_cast<msc::hud::HudMode>(mode);
    if (wanted == hudMode_) {
        return;
    }
    hudMode_ = wanted;
    channel_->hudMode.store(wanted, std::memory_order_relaxed);
    emit hudChanged();
}

QString VideoItem::hudStatus() const {
    using msc::hud::HudMode;
    if (hudMode_ == HudMode::Off) {
        return tr("off");
    }
    if (hudState_ == msc::hud::HudState::Shown) {
        return hudMode_ == HudMode::Synced ? tr("on the picture's own time")
                                           : tr("naive: newest pose, not the picture's");
    }
    return tr("hidden: %1").arg(QString::fromLatin1(msc::hud::toString(hudState_)));
}

void VideoItem::setFrameSource(std::shared_ptr<FrameSource> source, const msc::Clock& clock) {
    if (source == source_ && &clock == clock_) {
        return;
    }
    source_ = std::move(source);
    clock_ = &clock;
    update();  // kick the renderer; from then on it keeps itself going
}

void VideoItem::resetStats() {
    // Relaxed is enough: the flag carries no other data with it, and the renderer only needs
    // to see it eventually (at its next swap).
    channel_->resetRequested.store(true, std::memory_order_relaxed);
    // Drop anything published before the reset, so stats() never mixes old and new.
    msc::video::PresentationStats discarded;
    static_cast<void>(channel_->stats.read(discarded));
    stats_ = {};
    emit statsChanged();
}

QQuickRhiItemRenderer* VideoItem::createRenderer() {
    return new VideoRenderer();  // owned and deleted by Qt Quick, on the render thread
}
