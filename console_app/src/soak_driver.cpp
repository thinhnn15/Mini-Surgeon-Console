#include "soak_driver.hpp"

#include "fault_injector.hpp"
#include "graphics_api.hpp"
#include "msc/common/clock.hpp"
#include "msc/soak/fault_cycle.hpp"
#include "teleop_model.hpp"
#include "video_item.hpp"

#include <rhi/qrhi.h>
#include <QCoreApplication>
#include <QObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;
using msc::soak::Action;
using msc::soak::Injection;

constexpr int kSkipExitCode = 77;
constexpr int kInvalidExitCode = 3;
// A window not shown for this long invalidates the run (see checkShown).
constexpr msc::Nanoseconds kNotShownLimit = 500ms;
constexpr auto kStepPeriod = 20ms;
constexpr msc::Nanoseconds kSwapWindow = 1s;

double seconds(msc::Nanoseconds t) {
    return std::chrono::duration<double>(t).count();
}

double ms(msc::Nanoseconds t) {
    return std::chrono::duration<double, std::milli>(t).count();
}

unsigned long long u(std::uint64_t value) {
    return static_cast<unsigned long long>(value);
}

class SoakDriver : public QObject {
public:
    SoakDriver(QQuickWindow& window, VideoItem& video, TeleopModel& teleop, FaultInjector& faults,
               const msc::Clock& clock, SoakOptions options)
        : QObject(&window),
          window_(window),
          video_(video),
          teleop_(teleop),
          faults_(faults),
          clock_(clock),
          options_(options),
          started_(clock.now()),
          nextReport_(started_ + options.interval) {
        std::printf("soak: %lld s, a line every %lld s; faults in turn: link, corrupt, hand, video, fault\n",
                    static_cast<long long>(options.duration.count()),
                    static_cast<long long>(options.interval.count()));
        std::fflush(stdout);
        // Counted on the render thread: whether the window is still presenting at all, which is
        // the first thing to know when the picture stops (a compositor stops a hidden window).
        connect(
            &window, &QQuickWindow::frameSwapped, this,
            [this] { swaps_.fetch_add(1, std::memory_order_relaxed); }, Qt::DirectConnection);
        timer_.setInterval(kStepPeriod);
        connect(&timer_, &QTimer::timeout, this, [this] { step(); });
        timer_.start();
    }

private:
    void step() {
        if (finished_ || !checkDevice()) {
            return;
        }
        const msc::Nanoseconds now = clock_.now();
        if (!checkShown(now)) {
            return;
        }
        if (const auto* status = teleop_.coreStatus()) {
            apply(cycle_.step({.now = now - started_,
                               .state = status->state,
                               .fault = status->fault,
                               .engageAllowed = status->engageAllowed,
                               .resetAllowed = status->resetAllowed}));
        }
        if (cycle_.events() != eventsPrinted_) {
            eventsPrinted_ = cycle_.events();
            std::printf("soak event t=%.1f %s [window %s, %s, %llu frames presented in the last %.1f s]\n",
                        seconds(now - started_), cycle_.lastEvent().c_str(),
                        window_.isExposed() ? "exposed" : "NOT exposed",
                        window_.isActive() ? "active" : "not active", u(swapsLastSecond_),
                        seconds(kSwapWindow));
            std::fflush(stdout);
        }
        if (now >= nextSwapCount_) {
            nextSwapCount_ = now + kSwapWindow;
            const std::uint64_t swaps = swaps_.load(std::memory_order_relaxed);
            swapsLastSecond_ = swaps - swapsAtLastCount_;
            swapsAtLastCount_ = swaps;
        }
        if (now >= nextReport_) {
            nextReport_ += options_.interval;
            report("soak", now);
        }
        if (now - started_ >= options_.duration) {
            finish(now);
        }
    }

    // The software backend draws nothing; a graphics device that is the CPU starves the deadline
    // threads (journal M5.4): neither says anything about a long session on a real console.
    bool checkDevice() {
        if (deviceChecked_) {
            return true;
        }
        if (window_.rendererInterface()->graphicsApi() == QSGRendererInterface::Software) {
            std::printf("soak: FAIL - the software backend cannot draw the video item\n");
            end(1);
            return false;
        }
        const QRhi* rhi = window_.rhi();
        if (rhi == nullptr) {
            return false;  // not drawn yet
        }
        deviceChecked_ = true;
        std::printf("soak: drawing on \"%s\"\n", rhi->driverInfo().deviceName.constData());
        if (drawsOnTheCpu(rhi->driverInfo())) {
            std::printf("soak: SKIPPED - this machine draws on the CPU (%s); the soak test needs a GPU\n",
                        rhi->driverInfo().deviceName.constData());
            end(kSkipExitCode);
            return false;
        }
        std::fflush(stdout);
        return true;
    }

    // A compositor stops drawing a window nobody can see (GNOME on Wayland stops its frame
    // callbacks when it is covered; Qt then calls it not exposed). The picture is then really not
    // live, and the console faults, rightly (REQ-VID-003). That is not something wrong with the
    // console, and the rest of the run would measure nothing: say so and stop.
    bool checkShown(msc::Nanoseconds now) {
        if (window_.isExposed()) {
            notShownSince_.reset();
            return true;
        }
        if (!notShownSince_) {
            notShownSince_ = now;
        }
        if (now - *notShownSince_ < kNotShownLimit) {
            return true;
        }
        report("soak", now);
        std::printf(
            "soak: INVALID - the window has not been shown for %.1f s (t=%.1f): the compositor stops "
            "drawing a window that is covered, minimised or on another workspace, so the picture is not "
            "live. Keep the console window visible for the whole run.\n",
            seconds(now - *notShownSince_), seconds(now - started_));
        end(kInvalidExitCode);
        return false;
    }

    void apply(const Action& action) {
        switch (action.kind) {
            case Action::Kind::None:
                break;
            case Action::Kind::Engage:
                teleop_.engage();
                break;
            case Action::Kind::Reset:
                teleop_.reset();
                break;
            case Action::Kind::Inject:
            case Action::Kind::Clear:
                inject(action.injection, action.kind == Action::Kind::Inject);
                break;
        }
    }

    void inject(Injection injection, bool on) {
        switch (injection) {
            case Injection::Link:
                faults_.setLink(on);
                break;
            case Injection::Corrupt:
                faults_.setCorrupt(on);
                break;
            case Injection::Hand:
                faults_.setHand(on);
                break;
            case Injection::Video:
                faults_.setVideo(on);
                break;
            case Injection::Generic:
                faults_.setFault(on);
                break;
        }
    }

    void report(const char* head, msc::Nanoseconds now) {
        const auto& c = cycle_.counts();
        const auto* status = teleop_.coreStatus();
        const std::uint64_t lost = status != nullptr ? status->link.lost : 0;
        const std::uint64_t refused = status != nullptr ? status->link.refused : 0;
        std::printf(
            "%s t=%.1f phase=%s injected=%llu detected=%llu wrong=%llu missed=%llu early_reset=%llu "
            "recovered=%llu unexpected=%llu stuck=%llu p50_ms=%.2f p99_ms=%.2f render_fps=%.1f "
            "video_fps=%.1f "
            "dropped=%lld link_lost=%llu link_refused=%llu presented=%llu exposed=%d\n",
            head, seconds(now - started_), std::string(cycle_.phaseName()).c_str(), u(c.injected),
            u(c.detected), u(c.wrongFault), u(c.missed), u(c.earlyReset), u(c.recovered), u(c.unexpected),
            u(c.stuck), video_.latencyP50Ms(), video_.latencyP99Ms(), video_.renderFps(), video_.videoFps(),
            static_cast<long long>(video_.framesDropped()), u(lost), u(refused), u(swapsLastSecond_),
            window_.isExposed() ? 1 : 0);
        std::fflush(stdout);
    }

    void finish(msc::Nanoseconds now) {
        for (const auto injection : msc::soak::kInjections) {
            inject(injection, false);
        }
        report("soak done", now);
        const auto& c = cycle_.counts();
        if (!c.detectionTimes.empty()) {
            auto sorted = c.detectionTimes;
            std::ranges::sort(sorted);
            std::printf("soak: fault detected after p50 %.0f ms, max %.0f ms (%zu faults)\n",
                        ms(sorted[sorted.size() / 2]), ms(sorted.back()), sorted.size());
        }
        std::printf("soak: %s\n", c.clean()
                                      ? "PASS - the fault cycle stayed clean"
                                      : "FAIL - something happened that should not have (see the events)");
        end(c.clean() ? 0 : 1);
    }

    void end(int code) {
        std::fflush(stdout);
        finished_ = true;
        timer_.stop();
        QCoreApplication::exit(code);
    }

    QQuickWindow& window_;
    VideoItem& video_;
    TeleopModel& teleop_;
    FaultInjector& faults_;
    const msc::Clock& clock_;
    SoakOptions options_;
    msc::Nanoseconds started_;
    msc::Nanoseconds nextReport_;
    msc::soak::FaultCycle cycle_;
    std::uint64_t eventsPrinted_ = 0;
    std::atomic<std::uint64_t> swaps_{0};  // written on the render thread
    std::uint64_t swapsAtLastCount_ = 0;
    std::uint64_t swapsLastSecond_ = 0;
    msc::Nanoseconds nextSwapCount_{0};
    std::optional<msc::Nanoseconds> notShownSince_;
    bool deviceChecked_ = false;
    bool finished_ = false;
    QTimer timer_;
};

}  // namespace

void startSoak(QQuickWindow& window, VideoItem& video, TeleopModel& teleop, FaultInjector& faults,
               const msc::Clock& clock, SoakOptions options) {
    new SoakDriver(window, video, teleop, faults, clock, options);  // owned by the window
}
