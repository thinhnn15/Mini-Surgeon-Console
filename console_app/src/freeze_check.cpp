#include "freeze_check.hpp"

#include "graphics_api.hpp"
#include "msc/video/frame_source.hpp"
#include "msc/video/video_signal.hpp"
#include "video_item.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QQuickWindow>
#include <QTimer>

#include <chrono>
#include <cstdio>

namespace {

using namespace std::chrono_literals;
using State = VideoItem::VideoState;

// Window up, swap chain created, first frames drawn. Generous: CI machines are slow to start.
constexpr auto kStartTimeout = 15s;
// How long the picture must stay Live, without one false alarm, before the source is stopped.
constexpr auto kLiveFor = 1500ms;
// The promise under test is about the picture's age: flagged once the newest frame on screen is
// older than kFrozenAfter, give or take one watchdog period (50 ms). So the check reads the
// frame's age at the moment it was flagged, not the time since the stop: that would also count
// how old the frame already was at the stop, which depends on the display's refresh rate and
// the machine, not on the watchdog. The margin covers a GUI thread that is late for its timer:
// whenever QML changes (the statistics text, 4 times a second) the GUI thread waits for the
// render thread to sync, and with vsync at a low refresh rate that can be a couple of frames.
// Measured over Remote Desktop at 32 Hz: flagged at 284..395 ms of age.
constexpr auto kMaxAgeWhenFlagged = msc::video::kFrozenAfter + 250ms;
// From the stop: only a bound against "never", the age check above is the precise one.
constexpr auto kFlagWithin = 2s;
// Back to Live after the source starts again.
constexpr auto kRecoverWithin = 1s;
constexpr auto kPollPeriod = 10ms;

class FreezeCheck : public QObject {
public:
    FreezeCheck(QQuickWindow& window, VideoItem& item, msc::video::FrameSource& source)
        : QObject(&window), window_(window), item_(item), source_(source) {
        connect(&timer_, &QTimer::timeout, this, &FreezeCheck::poll);
        timer_.start(kPollPeriod);
        phaseTime_.start();
    }

private:
    enum class Phase { WaitForLive, StayLive, WaitForFrozen, WaitForLiveAgain };

    void poll() {
        const State state = item_.videoState();
        const auto elapsed = std::chrono::milliseconds(phaseTime_.elapsed());
        switch (phase_) {
            case Phase::WaitForLive:
                if (state == State::Live) {
                    next(Phase::StayLive);
                } else if (elapsed > kStartTimeout) {
                    fail("the picture never became live (no frame reached the screen)");
                }
                break;
            case Phase::StayLive:
                if (state != State::Live) {
                    fail("false alarm: the picture was flagged while the source was running");
                } else if (elapsed >= kLiveFor) {
                    source_.stop();
                    std::printf("freeze check: live for %lld ms, source stopped\n",
                                static_cast<long long>(elapsed.count()));
                    next(Phase::WaitForFrozen);
                }
                break;
            case Phase::WaitForFrozen:
                if (state == State::Frozen) {
                    const double ageMs = item_.frameAgeMs();
                    const double limitMs =
                        std::chrono::duration<double, std::milli>(kMaxAgeWhenFlagged).count();
                    std::printf(
                        "freeze check: flagged frozen %lld ms after the stop, picture %.0f ms old "
                        "(limit %.0f ms)\n",
                        static_cast<long long>(elapsed.count()), ageMs, limitMs);
                    if (ageMs > limitMs) {
                        fail("flagged too late: the picture was older than the limit");
                        return;
                    }
                    source_.start();
                    next(Phase::WaitForLiveAgain);
                } else if (elapsed > kFlagWithin) {
                    fail("the stopped source was never flagged as frozen");
                }
                break;
            case Phase::WaitForLiveAgain:
                if (state == State::Live) {
                    std::printf("freeze check: live again %lld ms after the restart\nPASS\n",
                                static_cast<long long>(elapsed.count()));
                    finish(0);
                } else if (elapsed > kRecoverWithin) {
                    fail("did not become live again after the source restarted");
                }
                break;
        }
    }

    void next(Phase phase) {
        phase_ = phase;
        phaseTime_.restart();
    }

    void fail(const char* reason) {
        std::printf("freeze check: FAIL - %s (graphics API %s)\n", reason,
                    graphicsApiName(window_.rendererInterface()->graphicsApi()));
        finish(1);
    }

    void finish(int code) {
        timer_.stop();
        std::fflush(stdout);
        QCoreApplication::exit(code);
    }

    QQuickWindow& window_;
    VideoItem& item_;
    msc::video::FrameSource& source_;
    QTimer timer_;
    QElapsedTimer phaseTime_;
    Phase phase_ = Phase::WaitForLive;
};

}  // namespace

void startFreezeCheck(QQuickWindow& window, VideoItem& item, msc::video::FrameSource& source) {
    new FreezeCheck(window, item, source);  // owned by the window
}
