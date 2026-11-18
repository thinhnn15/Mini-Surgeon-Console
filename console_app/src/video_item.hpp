#pragma once

#include "msc/common/clock.hpp"
#include "msc/common/latest_value.hpp"
#include "msc/video/frame_source.hpp"
#include "msc/video/presentation_tracker.hpp"
#include "msc/video/video_signal.hpp"

#include <QtQml/qqmlregistration.h>
#include <QQuickRhiItem>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>

class VideoRenderer;

// How the renderer's statistics reach the GUI thread. Not through synchronize(): the renderer
// keeps itself rendering with QQuickRhiItemRenderer::update(), which on purpose does not run
// synchronize() (continuous rendering must not block the GUI thread every frame), so
// synchronize() only happens when the GUI side changes something. A lock-free mailbox
// (REQ-VID-001's LatestValue) works whatever Qt does: the render thread publishes, the GUI
// thread reads on a timer. Shared, because the renderer may outlive the item.
struct VideoStatsChannel {
    static constexpr std::int64_t kNoFrame = std::numeric_limits<std::int64_t>::min();

    msc::LatestValue<msc::video::PresentationStats> stats;  // render thread -> GUI thread
    std::atomic<bool> resetRequested{false};                // GUI thread -> render thread
    // REQ-VID-003: Frame::captureTime (ns) of the newest frame drawn, or kNoFrame. Updated on
    // every new frame, not every 250 ms like the statistics, so the frozen-picture watchdog
    // reacts within its own period instead of waiting for the next statistics.
    std::atomic<std::int64_t> newestCapture{kNoFrame};  // render thread -> GUI thread
};

// Shows the newest frame of a FrameSource, scaled to fit at its own aspect ratio (black bars
// around it). Drawn with QRhi on the Qt Quick render thread; see ADR 0003.
//
// Threads:
//   - GUI thread: the item itself, setFrameSource(), the statistics properties.
//   - Render thread: the renderer. It is the source's one reader: it calls acquireLatest()
//     once per rendered frame and uploads straight from latest(), without a copy. It also
//     times every frame along the path (capture, upload, present) for REQ-VID-002.
//   The source goes to the renderer in synchronize(), while the GUI thread is blocked (Qt's
//   safe meeting point); statistics come back through a VideoStatsChannel.
//
// The item renders continuously (every vsync) while it has a source: a video view has a new
// picture nearly every frame, so there is nothing to gain from waiting to be asked.
class VideoItem : public QQuickRhiItem {
    Q_OBJECT
    QML_ELEMENT

    // Presentation statistics for the HUD (REQ-VID-002). Refreshed a few times per second:
    // a person cannot read faster than that, and QML bindings are not free.
    Q_PROPERTY(int latencySamples READ latencySamples NOTIFY statsChanged FINAL)
    Q_PROPERTY(double latencyP50Ms READ latencyP50Ms NOTIFY statsChanged FINAL)
    Q_PROPERTY(double latencyP99Ms READ latencyP99Ms NOTIFY statsChanged FINAL)
    Q_PROPERTY(double latencyMaxMs READ latencyMaxMs NOTIFY statsChanged FINAL)
    Q_PROPERTY(double renderFps READ renderFps NOTIFY statsChanged FINAL)
    Q_PROPERTY(double videoFps READ videoFps NOTIFY statsChanged FINAL)
    Q_PROPERTY(qint64 framesShown READ framesShown NOTIFY statsChanged FINAL)
    Q_PROPERTY(qint64 framesDropped READ framesDropped NOTIFY statsChanged FINAL)

    // REQ-VID-003: is the picture live? Checked every kWatchdogPeriod on the GUI thread.
    Q_PROPERTY(VideoState videoState READ videoState NOTIFY videoStateChanged FINAL)
    // Age of the newest frame on screen; refreshed while the picture is not live.
    Q_PROPERTY(double frameAgeMs READ frameAgeMs NOTIFY frameAgeChanged FINAL)
    // What the source is, for the operator ("Logi C310, 640x480 YUYV", "synthetic pattern",
    // "camera disconnected"). Set by whoever owns the source.
    Q_PROPERTY(QString sourceStatus READ sourceStatus WRITE setSourceStatus NOTIFY sourceStatusChanged FINAL)

public:
    enum class VideoState : std::uint8_t {
        Waiting = static_cast<std::uint8_t>(msc::video::VideoSignal::Waiting),
        Live = static_cast<std::uint8_t>(msc::video::VideoSignal::Live),
        Frozen = static_cast<std::uint8_t>(msc::video::VideoSignal::Frozen),
    };
    Q_ENUM(VideoState)

    explicit VideoItem(QQuickItem* parent = nullptr);

    // GUI thread. Shared ownership: the render thread may still hold the old source for a
    // moment after this call, and must never see it destroyed under its feet. `clock` must be
    // the clock the source stamps Frame::captureTime with, and must outlive the item.
    void setFrameSource(std::shared_ptr<msc::video::FrameSource> source, const msc::Clock& clock);
    [[nodiscard]] const std::shared_ptr<msc::video::FrameSource>& frameSource() const noexcept {
        return source_;
    }

    // GUI thread. The statistics as of the last refresh (at most a quarter second old).
    [[nodiscard]] const msc::video::PresentationStats& stats() const noexcept { return stats_; }
    // GUI thread. Starts the statistics again from zero, for example after a warm-up.
    void resetStats();

    [[nodiscard]] int latencySamples() const noexcept { return static_cast<int>(stats_.samples); }
    [[nodiscard]] double latencyP50Ms() const noexcept { return toMs(stats_.captureToPresentP50); }
    [[nodiscard]] double latencyP99Ms() const noexcept { return toMs(stats_.captureToPresentP99); }
    [[nodiscard]] double latencyMaxMs() const noexcept { return toMs(stats_.captureToPresentMax); }
    [[nodiscard]] double renderFps() const noexcept { return stats_.renderFps; }
    [[nodiscard]] double videoFps() const noexcept { return stats_.videoFps; }
    [[nodiscard]] qint64 framesShown() const noexcept { return static_cast<qint64>(stats_.framesShown); }
    [[nodiscard]] qint64 framesDropped() const noexcept { return static_cast<qint64>(stats_.framesDropped); }

    [[nodiscard]] VideoState videoState() const noexcept { return videoState_; }
    [[nodiscard]] double frameAgeMs() const noexcept { return frameAgeMs_; }
    [[nodiscard]] const QString& sourceStatus() const noexcept { return sourceStatus_; }
    void setSourceStatus(const QString& status);

signals:
    void statsChanged();
    void videoStateChanged();
    void frameAgeChanged();
    void sourceStatusChanged();

protected:
    QQuickRhiItemRenderer* createRenderer() override;

private:
    friend class VideoRenderer;  // reads source_, clock_ and channel_, in synchronize() only

    // 0 when there is no sample yet; QML checks latencySamples first.
    static double toMs(const std::optional<msc::Nanoseconds>& t) noexcept {
        return t ? std::chrono::duration<double, std::milli>(*t).count() : 0.0;
    }
    void checkVideoSignal();

    std::shared_ptr<msc::video::FrameSource> source_;
    const msc::Clock* clock_ = nullptr;
    std::shared_ptr<VideoStatsChannel> channel_ = std::make_shared<VideoStatsChannel>();
    msc::video::PresentationStats stats_;  // GUI thread's copy, read from channel_
    QTimer statsTimer_;                    // reads channel_ and tells QML
    QTimer watchdogTimer_;                 // REQ-VID-003, see checkVideoSignal()
    VideoState videoState_ = VideoState::Waiting;
    double frameAgeMs_ = 0.0;
    QString sourceStatus_;
};
