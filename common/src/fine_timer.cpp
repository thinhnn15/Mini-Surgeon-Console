#include "msc/common/fine_timer.hpp"

#include <chrono>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: timeapi.h depends on it.
#include <timeapi.h>
#endif

namespace msc {

#if defined(_WIN32)
FineTimerResolution::FineTimerResolution() noexcept {
    // Windows 11 ignores a process's timer resolution request while the process has no visible
    // window (power throttling), and arm_sim never has one: measured, its 1 kHz loop ran at
    // 635 Hz with ticks up to 15.6 ms late. This opts the process out of that one throttle.
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    throttling.StateMask = 0;  // in the mask, not set: do not ignore our request
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &throttling, sizeof(throttling));
    timeBeginPeriod(1);
}
FineTimerResolution::~FineTimerResolution() {
    timeEndPeriod(1);
}

namespace {

// One high-resolution waitable timer per thread, created on first use, closed at thread exit.
class ThreadTimer {
public:
    ThreadTimer() noexcept
        : handle_(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                         TIMER_ALL_ACCESS)) {}
    ~ThreadTimer() {
        if (handle_ != nullptr) {
            CloseHandle(handle_);
        }
    }
    ThreadTimer(const ThreadTimer&) = delete;
    ThreadTimer& operator=(const ThreadTimer&) = delete;
    ThreadTimer(ThreadTimer&&) = delete;
    ThreadTimer& operator=(ThreadTimer&&) = delete;

    [[nodiscard]] HANDLE handle() const noexcept { return handle_; }

private:
    HANDLE handle_;
};

}  // namespace

void sleepUntil(std::chrono::steady_clock::time_point deadline) noexcept {
    const auto remaining = deadline - std::chrono::steady_clock::now();
    if (remaining <= std::chrono::steady_clock::duration::zero()) {
        return;
    }
    thread_local const ThreadTimer timer;
    if (timer.handle() == nullptr) {  // older Windows: no high-resolution timers
        std::this_thread::sleep_until(deadline);
        return;
    }
    // Due time in 100 ns units; negative means "relative to now".
    LARGE_INTEGER due{};
    due.QuadPart =
        -std::chrono::duration_cast<std::chrono::duration<long long, std::ratio<1, 10'000'000>>>(remaining)
             .count();
    if (SetWaitableTimerEx(timer.handle(), &due, 0, nullptr, nullptr, nullptr, 0) == 0) {
        std::this_thread::sleep_until(deadline);
        return;
    }
    WaitForSingleObject(timer.handle(), INFINITE);
}
#else
FineTimerResolution::FineTimerResolution() noexcept = default;
FineTimerResolution::~FineTimerResolution() = default;

void sleepUntil(std::chrono::steady_clock::time_point deadline) noexcept {
    std::this_thread::sleep_until(deadline);
}
#endif

}  // namespace msc
