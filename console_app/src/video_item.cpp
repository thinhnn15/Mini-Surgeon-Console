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

// Bars around the picture. Black, as on a real endoscope monitor: anything brighter pulls
// the eye away from the image.
const QColor kBarColour = Qt::black;

// Qt sizes are int; letterbox() works in unsigned pixels. Sizes are never negative.
std::uint32_t pixels(int length) {
    return static_cast<std::uint32_t>(std::max(length, 0));
}

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
    // Render thread, right after the window's swap (Present) has returned.
    void onSwapped();

    std::shared_ptr<FrameSource> source_;
    const msc::Clock* clock_ = nullptr;
    bool hasFrame_ = false;  // a frame has been uploaded since the source was set

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
    }
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

void VideoRenderer::render(QRhiCommandBuffer* cb) {
    QRhiResourceUpdateBatch* updates = nullptr;
    // Exactly one acquire per rendered frame (ADR 0002): the frame stays valid until the next.
    if (pipeline_ && source_ && source_->acquireLatest()) {
        const Frame& frame = source_->latest();
        const QSize size(static_cast<int>(frame.width), static_cast<int>(frame.height));
        if (frame.format == msc::video::PixelFormat::Rgba8 && !frame.empty() && ensureTextureSize(size)) {
            updates = rhi_->nextResourceUpdateBatch();
            uploadFrame(frame, updates);
            hasFrame_ = true;
            if (clock_ != nullptr) {
                tracker_.frameUploaded(frame.sequence, frame.captureTime, clock_->now());
            }
        }
    }

    cb->beginPass(renderTarget(), kBarColour, {1.0F, 0}, updates);
    if (hasFrame_) {
        const QSize target = renderTarget()->pixelSize();
        const QSize image = texture_->pixelSize();
        const msc::video::PixelRect box = msc::video::letterbox(
            pixels(image.width()), pixels(image.height()), pixels(target.width()), pixels(target.height()));
        // QRhiViewport counts y from the bottom (OpenGL style); PixelRect counts from the top.
        const std::uint32_t fromBottom = pixels(target.height()) - box.y - box.height;
        cb->setGraphicsPipeline(pipeline_.get());
        cb->setViewport(QRhiViewport(static_cast<float>(box.x), static_cast<float>(fromBottom),
                                     static_cast<float>(box.width), static_cast<float>(box.height)));
        cb->setShaderResources();
        const QRhiCommandBuffer::VertexInput input(vertices_.get(), 0);
        cb->setVertexInput(0, 1, &input);
        cb->draw(4);
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
