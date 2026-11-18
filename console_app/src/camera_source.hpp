#pragma once

#include "msc/common/clock.hpp"
#include "msc/video/frame.hpp"
#include "msc/video/frame_source.hpp"

#include <QCameraDevice>
#include <QCameraFormat>
#include <QMediaCaptureSession>
#include <QMediaDevices>
#include <QObject>
#include <QSize>
#include <QString>

#include <cstdint>
#include <memory>
#include <optional>
#include <thread>

class QCamera;
class QVideoSink;

// A real camera as a FrameSource (M2.5), through Qt Multimedia: QCamera -> QMediaCaptureSession
// -> QVideoSink. Each QVideoFrame is converted to RGBA and published through the same
// TripleBuffer as the synthetic source, so VideoItem does not know or care which one it has.
//
// Threads:
//   - Owning thread (the GUI thread): construction, start(), stop(), destruction, and every
//     Qt Multimedia object. The camera's own signals (errors, unplugged) arrive here.
//   - Delivery thread: wherever the backend emits QVideoSink::videoFrameChanged. That depends
//     on the backend; with Qt 6.8 on Windows it is the GUI thread (measured, see the journal).
//     Delivery only stamps the arrival time and puts the frame (a reference-counted handle,
//     no pixels copied) into a one-frame hand-off slot.
//   - Camera thread (ours): takes the newest frame from the slot, converts it to RGBA and
//     publishes it. It is the TripleBuffer's only writer, whatever thread the backend uses,
//     and the conversion never runs on the GUI thread (in a Debug build it takes longer than
//     the camera's frame period: it would leave the GUI thread no time for anything else).
//   - Reader thread (the renderer): acquireLatest() / latest(), as for any FrameSource.
//
// The hand-off slot and the statistics have mutexes; the renderer takes neither and stays
// lock-free. Once stop() returns the camera thread has been joined: no frame is being written.
//
// "Capture time" is when the frame reached this process. What happened before (exposure,
// the camera's own processing, USB, the driver) is not visible from here. Measuring that
// means pointing the camera at a clock on the screen: glass-to-glass.
class CameraSource final : public QObject, public msc::video::FrameSource {
    Q_OBJECT

public:
    // `clock` stamps Frame::captureTime and must outlive the source.
    CameraSource(const msc::Clock& clock, const QCameraDevice& device, QObject* parent = nullptr);
    // Owning thread only (it deletes Qt Multimedia objects). Keep a reference on the owning
    // thread, so the last one is not dropped by the renderer on the render thread.
    ~CameraSource() override;

    CameraSource(const CameraSource&) = delete;
    CameraSource& operator=(const CameraSource&) = delete;
    CameraSource(CameraSource&&) = delete;
    CameraSource& operator=(CameraSource&&) = delete;

    void start() override;
    void stop() override;
    [[nodiscard]] bool acquireLatest() noexcept override;
    [[nodiscard]] const msc::video::Frame& latest() const noexcept override;

    // Owning thread. One line for the operator: device and format, or what went wrong.
    [[nodiscard]] const QString& status() const noexcept { return status_; }

    // Owning thread. What the camera delivers and what converting it costs.
    struct Report {
        QString pixelFormat;         // as the camera sends it, e.g. "YUYV"
        QSize size;                  // pixels
        bool viaImage = false;       // no direct converter: went through QVideoFrame::toImage()
        std::uint64_t frames = 0;    // converted and published
        std::uint64_t replaced = 0;  // delivered but replaced by a newer one before conversion
        bool deliveredOnGuiThread = false;
        std::optional<msc::Nanoseconds> conversionP50;
        std::optional<msc::Nanoseconds> conversionP99;
    };
    [[nodiscard]] Report report() const;

    // The camera to use: the first whose description or id contains `wanted` (case does not
    // matter), or the system default if `wanted` is empty. Nothing if there is none.
    [[nodiscard]] static std::optional<QCameraDevice> findDevice(const QString& wanted);
    // Prints every camera and the formats it offers (for --list-cameras).
    static void listDevices();
    // The format this source asks for: see the .cpp for the rule.
    [[nodiscard]] static QCameraFormat chooseFormat(const QCameraDevice& device);

signals:
    void statusChanged(const QString& status);

private:
    struct Shared;

    void openCamera();
    void closeCamera();
    void setStatus(const QString& status);
    void onDevicesChanged();
    void onFirstFrame();

    std::shared_ptr<Shared> shared_;  // also held by the delivery callback
    std::jthread converter_;          // the camera thread, while started
    QCameraDevice device_;
    QMediaDevices devices_;  // tells us when cameras come and go
    QMediaCaptureSession session_;
    std::unique_ptr<QVideoSink> sink_;
    std::unique_ptr<QCamera> camera_;
    bool started_ = false;  // owning thread: start() called and not stopped since
    QString status_;
};
