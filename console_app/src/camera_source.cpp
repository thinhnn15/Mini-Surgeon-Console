#include "camera_source.hpp"

#include "msc/common/latency_stats.hpp"
#include "msc/common/triple_buffer.hpp"
#include "msc/video/pixel_convert.hpp"

#include <QCamera>
#include <QCoreApplication>
#include <QImage>
#include <QThread>
#include <QVideoFrame>
#include <QVideoFrameFormat>
#include <QVideoSink>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <stop_token>
#include <tuple>

namespace {

using msc::video::Frame;
using msc::video::Plane;
using msc::video::YuvEncoding;

// Formats converted by msc::video's own code. Anything else goes through
// QVideoFrame::toImage(), which handles every format Qt knows (JPEG included) but costs more.
bool hasDirectConverter(QVideoFrameFormat::PixelFormat format) {
    switch (format) {
        case QVideoFrameFormat::Format_NV12:
        case QVideoFrameFormat::Format_YUYV:
        case QVideoFrameFormat::Format_UYVY:
        case QVideoFrameFormat::Format_BGRA8888:
        case QVideoFrameFormat::Format_BGRX8888:
        case QVideoFrameFormat::Format_RGBA8888:
        case QVideoFrameFormat::Format_RGBX8888:
            return true;
        default:
            return false;
    }
}

YuvEncoding encodingOf(const QVideoFrameFormat& format) {
    // Unknown is common: then BT.601 limited range, what nearly every webcam sends.
    return YuvEncoding{
        .matrix = format.colorSpace() == QVideoFrameFormat::ColorSpace_BT709 ? msc::video::YuvMatrix::Bt709
                                                                             : msc::video::YuvMatrix::Bt601,
        .range = format.colorRange() == QVideoFrameFormat::ColorRange_Full ? msc::video::YuvRange::Full
                                                                           : msc::video::YuvRange::Limited,
    };
}

// The writer's slot holds whatever it held three frames ago, possibly at another size.
// Allocates only when the size changes (the first frames, or a format change): never per frame.
void ensureSize(Frame& frame, int width, int height) {
    const auto w = static_cast<std::uint32_t>(width);
    const auto h = static_cast<std::uint32_t>(height);
    if (frame.width != w || frame.height != h) {
        frame = Frame::allocate(w, h);
    }
}

// False if there is no converter for the format, the frame cannot be mapped, or its planes are
// smaller than its size says (then the caller tries toImage()).
bool convertDirect(QVideoFrame& frame, Frame& out) {
    const QVideoFrameFormat::PixelFormat format = frame.pixelFormat();
    if (!hasDirectConverter(format) || !frame.map(QVideoFrame::ReadOnly)) {
        return false;
    }
    ensureSize(out, frame.width(), frame.height());
    // A plane the frame does not have comes out empty, and the converter refuses it.
    const auto plane = [&frame](int index) {
        const auto* bits = frame.bits(index);
        const auto size = static_cast<std::size_t>(std::max(frame.mappedBytes(index), 0));
        return Plane{.data = std::span<const std::uint8_t>(bits, bits != nullptr ? size : 0),
                     .stride = static_cast<std::size_t>(std::max(frame.bytesPerLine(index), 0))};
    };
    const YuvEncoding encoding = encodingOf(frame.surfaceFormat());
    // Ignored for now: QVideoFrame::rotation() / mirrored() and bottom-to-top scan lines. Webcams
    // in their YUV formats do not use them; toImage() below would apply them.
    bool converted = false;
    switch (format) {
        case QVideoFrameFormat::Format_NV12:
            converted = msc::video::nv12ToRgba(plane(0), plane(1), encoding, out);
            break;
        case QVideoFrameFormat::Format_YUYV:
            converted = msc::video::yuyvToRgba(plane(0), encoding, out);
            break;
        case QVideoFrameFormat::Format_UYVY:
            converted = msc::video::uyvyToRgba(plane(0), encoding, out);
            break;
        case QVideoFrameFormat::Format_BGRA8888:
        case QVideoFrameFormat::Format_BGRX8888:
            converted = msc::video::bgrxToRgba(plane(0), out);
            break;
        default:  // RGBA8888, RGBX8888
            converted = msc::video::rgbxToRgba(plane(0), out);
            break;
    }
    frame.unmap();
    return converted;
}

// Slow path: Qt converts (and decodes JPEG) into a QImage, then one more copy into the frame.
bool convertViaImage(const QVideoFrame& frame, Frame& out) {
    QImage image = frame.toImage();
    if (image.isNull()) {
        return false;
    }
    image.convertTo(QImage::Format_RGBA8888);
    ensureSize(out, image.width(), image.height());
    for (std::uint32_t row = 0; row < out.height; ++row) {
        std::memcpy(out.pixels.data() + (std::size_t{row} * out.strideBytes()),
                    image.constScanLine(static_cast<int>(row)), out.strideBytes());
    }
    return true;
}

QString formatName(QVideoFrameFormat::PixelFormat format) {
    QString name = QVideoFrameFormat::pixelFormatToString(format);
    // "Format_YUYV" reads better as "YUYV".
    if (name.startsWith(QLatin1String("Format_"))) {
        name.remove(0, 7);
    }
    return name;
}

// The most pixels the source asks for: the synthetic source's size. Converting 1080p on the
// CPU in a Debug build costs several milliseconds per frame, and 720p is plenty to learn on.
constexpr int kMaxArea = 1280 * 720;
// Below this a moving picture visibly stutters.
constexpr float kSmoothFps = 25.0F;

}  // namespace

// What the owner, the delivery callback and the camera thread share. Held through a
// shared_ptr by the callback too, so it outlives a delivery that is still running while the
// source is being destroyed.
struct CameraSource::Shared {
    explicit Shared(const msc::Clock& c) : clock(&c) {}

    void deliver(const QVideoFrame& frame);                                // delivery thread
    void convertLoop(const std::stop_token& stop);                         // camera thread
    void convertAndPublish(QVideoFrame& frame, msc::Nanoseconds arrived);  // camera thread

    const msc::Clock* clock;

    // Hand-off from delivery to the camera thread: the newest frame not converted yet. A slot
    // of one, like the mailbox to the renderer: if the camera thread falls behind, older frames
    // are replaced (and counted), never queued.
    std::mutex handoff;
    std::condition_variable_any wake;
    // Guarded by `handoff`.
    bool accepting = false;
    std::optional<QVideoFrame> pending;
    msc::Nanoseconds pendingArrived{0};
    std::uint64_t replaced = 0;
    bool deliveryThreadKnown = false;
    bool deliveredOnGuiThread = false;

    // Camera thread only (the TripleBuffer's one writer; the renderer is its one reader).
    // The buffer starts empty: the camera's size is only known once it delivers.
    std::uint64_t nextSequence = 1;
    msc::TripleBuffer<Frame> buffer;

    // Written by the camera thread, read by the owner. Guarded by `statsMutex`.
    std::mutex statsMutex;
    msc::LatencyStats conversion{600};
    Report report;
    bool firstFrameSeen = false;
    CameraSource* owner = nullptr;  // set before the camera thread starts
};

// Delivery thread (whichever the backend uses; the GUI thread on Windows). Only stamps the
// time and swaps a reference-counted handle: microseconds, whatever the frame size.
void CameraSource::Shared::deliver(const QVideoFrame& frame) {
    const msc::Nanoseconds arrived = clock->now();
    if (!frame.isValid()) {
        return;
    }
    {
        const std::scoped_lock lock(handoff);
        if (!accepting) {
            return;
        }
        if (pending) {
            ++replaced;
        }
        pending = frame;
        pendingArrived = arrived;
        if (!deliveryThreadKnown) {
            deliveryThreadKnown = true;
            deliveredOnGuiThread = QThread::currentThread() == QCoreApplication::instance()->thread();
        }
    }
    wake.notify_one();
}

// Camera thread.
void CameraSource::Shared::convertLoop(const std::stop_token& stop) {
    for (;;) {
        QVideoFrame frame;
        msc::Nanoseconds arrived{0};
        {
            std::unique_lock lock(handoff);
            // Wakes on a new frame or on stop() (the stop_token overload needs no extra notify).
            if (!wake.wait(lock, stop, [this] { return pending.has_value(); })) {
                return;
            }
            frame = std::move(*pending);
            pending.reset();
            arrived = pendingArrived;
        }
        convertAndPublish(frame, arrived);
    }
}

// Camera thread.
void CameraSource::Shared::convertAndPublish(QVideoFrame& frame, msc::Nanoseconds arrived) {
    const msc::Nanoseconds started = clock->now();
    Frame& out = buffer.writeSlot();
    const bool direct = convertDirect(frame, out);
    if (!direct && !convertViaImage(frame, out)) {
        return;  // nothing to show; the watchdog flags the picture if this goes on
    }
    out.format = msc::video::PixelFormat::Rgba8;
    out.sequence = nextSequence++;
    // Arrival, not now: the latency the console reports includes the hand-off and conversion.
    out.captureTime = arrived;
    buffer.publish();
    const msc::Nanoseconds finished = clock->now();

    const std::scoped_lock lock(statsMutex);
    // Map, convert, unmap (and on the slow path Qt's decoding): what the camera's format costs.
    conversion.add(finished - started);
    ++report.frames;
    report.pixelFormat = formatName(frame.pixelFormat());
    report.size = frame.size();
    report.viaImage = !direct;
    if (!firstFrameSeen) {
        firstFrameSeen = true;
        // Tell the owner on its own thread. Safe: the owner's destructor joins this thread (in
        // stop()) before it goes, and Qt drops events posted to an object it deletes.
        QMetaObject::invokeMethod(owner, &CameraSource::onFirstFrame, Qt::QueuedConnection);
    }
}

CameraSource::CameraSource(const msc::Clock& clock, const QCameraDevice& device, QObject* parent)
    : QObject(parent),
      shared_(std::make_shared<Shared>(clock)),
      device_(device),
      sink_(std::make_unique<QVideoSink>()) {
    shared_->owner = this;
    session_.setVideoSink(sink_.get());
    // Direct: hand the frame over on whatever thread delivers it (see the class comment). The
    // callback holds the shared state, not `this`.
    connect(
        sink_.get(), &QVideoSink::videoFrameChanged, sink_.get(),
        [shared = shared_](const QVideoFrame& frame) { shared->deliver(frame); }, Qt::DirectConnection);
    connect(&devices_, &QMediaDevices::videoInputsChanged, this, &CameraSource::onDevicesChanged);
    setStatus(tr("%1: stopped").arg(device_.description()));
}

CameraSource::~CameraSource() {
    stop();
    session_.setVideoSink(nullptr);
}

void CameraSource::start() {
    if (started_) {
        return;
    }
    started_ = true;
    {
        const std::scoped_lock lock(shared_->handoff);
        shared_->accepting = true;
        shared_->pending.reset();
    }
    Shared* shared = shared_.get();  // outlives the thread: stop() joins it
    converter_ = std::jthread([shared](const std::stop_token& stop) { shared->convertLoop(stop); });
    openCamera();
}

void CameraSource::stop() {
    if (!started_) {
        return;
    }
    started_ = false;
    {
        const std::scoped_lock lock(shared_->handoff);
        shared_->accepting = false;
        shared_->pending.reset();
    }
    // After the join no frame is being written or will be.
    converter_.request_stop();
    converter_.join();
    closeCamera();
    setStatus(tr("%1: stopped").arg(device_.description()));
}

bool CameraSource::acquireLatest() noexcept {
    return shared_->buffer.acquireLatest();
}

const Frame& CameraSource::latest() const noexcept {
    return shared_->buffer.readSlot();
}

CameraSource::Report CameraSource::report() const {
    Report r;
    {
        const std::scoped_lock lock(shared_->statsMutex);
        r = shared_->report;
        r.conversionP50 = shared_->conversion.p50();
        r.conversionP99 = shared_->conversion.p99();
    }
    const std::scoped_lock lock(shared_->handoff);
    r.replaced = shared_->replaced;
    r.deliveredOnGuiThread = shared_->deliveredOnGuiThread;
    return r;
}

void CameraSource::openCamera() {
    {
        const std::scoped_lock lock(shared_->statsMutex);
        shared_->firstFrameSeen = false;  // report the format again when frames (re)start
    }
    camera_ = std::make_unique<QCamera>(device_);
    const QCameraFormat format = chooseFormat(device_);
    if (!format.isNull()) {
        camera_->setCameraFormat(format);
    }
    connect(camera_.get(), &QCamera::errorOccurred, this, [this](QCamera::Error, const QString& message) {
        setStatus(tr("%1: error: %2").arg(device_.description(), message));
    });
    connect(camera_.get(), &QCamera::activeChanged, this, [this](bool active) {
        if (!active && started_) {
            setStatus(tr("%1: stopped delivering").arg(device_.description()));
        }
    });
    session_.setCamera(camera_.get());
    camera_->start();
    setStatus(tr("%1: starting").arg(device_.description()));
}

void CameraSource::closeCamera() {
    if (!camera_) {
        return;
    }
    camera_->disconnect(this);  // no "stopped delivering" for a stop we asked for
    session_.setCamera(nullptr);
    camera_->stop();
    camera_.reset();
}

void CameraSource::setStatus(const QString& status) {
    if (status == status_) {
        return;
    }
    status_ = status;
    qInfo("camera: %s", qPrintable(status_));
    emit statusChanged(status_);
}

// Unplugged: close the camera, keep waiting. Plugged back in: open it again. While it is gone
// the renderer keeps the last frame, and VideoItem's watchdog marks it frozen (REQ-VID-003).
void CameraSource::onDevicesChanged() {
    const QList<QCameraDevice> inputs = QMediaDevices::videoInputs();
    const auto match = std::find_if(inputs.cbegin(), inputs.cend(),
                                    [this](const QCameraDevice& d) { return d.id() == device_.id(); });
    if (match == inputs.cend()) {
        closeCamera();
        if (started_) {
            setStatus(tr("%1: disconnected, waiting for it to come back").arg(device_.description()));
        }
        return;
    }
    if (started_ && !camera_) {
        device_ = *match;
        openCamera();
    }
}

void CameraSource::onFirstFrame() {
    const Report r = report();
    qInfo("camera: frames are delivered on %s and converted on the camera thread",
          r.deliveredOnGuiThread ? "the GUI thread" : "a backend thread (not the GUI thread)");
    setStatus(tr("%1, %2x%3 %4%5")
                  .arg(device_.description())
                  .arg(r.size.width())
                  .arg(r.size.height())
                  .arg(r.pixelFormat, r.viaImage ? tr(" (converted by Qt)") : QString()));
}

std::optional<QCameraDevice> CameraSource::findDevice(const QString& wanted) {
    if (wanted.isEmpty()) {
        const QCameraDevice device = QMediaDevices::defaultVideoInput();
        return device.isNull() ? std::nullopt : std::optional<QCameraDevice>(device);
    }
    for (const QCameraDevice& device : QMediaDevices::videoInputs()) {
        if (device.description().contains(wanted, Qt::CaseInsensitive) ||
            QString::fromUtf8(device.id()).contains(wanted, Qt::CaseInsensitive)) {
            return device;
        }
    }
    return std::nullopt;
}

// Smooth first (at least kSmoothFps), then formats we convert ourselves (no JPEG decoding:
// cheaper and no compression delay), then the most pixels up to kMaxArea, then the higher
// frame rate.
QCameraFormat CameraSource::chooseFormat(const QCameraDevice& device) {
    QCameraFormat best;
    auto rank = [](const QCameraFormat& f) {
        const int area = f.resolution().width() * f.resolution().height();
        return std::make_tuple(f.maxFrameRate() >= kSmoothFps, area <= kMaxArea,
                               hasDirectConverter(f.pixelFormat()), area <= kMaxArea ? area : -area,
                               f.maxFrameRate());
    };
    for (const QCameraFormat& format : device.videoFormats()) {
        if (best.isNull() || rank(format) > rank(best)) {
            best = format;
        }
    }
    return best;
}

void CameraSource::listDevices() {
    const QList<QCameraDevice> inputs = QMediaDevices::videoInputs();
    if (inputs.isEmpty()) {
        std::printf("No camera found.\n");
    }
    for (const QCameraDevice& device : inputs) {
        std::printf("%s%s\n  id: %s\n", qPrintable(device.description()),
                    device.isDefault() ? " (default)" : "", device.id().constData());
        const QCameraFormat chosen = chooseFormat(device);
        for (const QCameraFormat& f : device.videoFormats()) {
            std::printf("  %4dx%-4d %-10s %5.1f-%4.1f fps%s\n", f.resolution().width(),
                        f.resolution().height(), qPrintable(formatName(f.pixelFormat())),
                        static_cast<double>(f.minFrameRate()), static_cast<double>(f.maxFrameRate()),
                        f == chosen ? "  <- used" : "");
        }
    }
    std::fflush(stdout);
}
