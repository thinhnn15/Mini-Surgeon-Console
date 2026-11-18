#pragma once

#include <chrono>

namespace msc {

// Asks the OS for 1 ms timer resolution while the object lives (RAII), for threads that sleep
// until a deadline: the video producer (60 fps) and the arm's control loop (1 kHz).
//
// Windows wakes sleeping threads on a ~15.6 ms timer tick by default, so a 16.7 ms frame
// period came out as 15.6 or 31.2 ms (M2.2: gaps of 15.0-32.2 ms at 60 fps; 15.0-18.6 ms with
// a 1 ms tick), and a 1 ms period is impossible. The request is per process on Windows 10 2004
// and later. Elsewhere it does nothing: Linux timers are already fine-grained.
class FineTimerResolution {
public:
    FineTimerResolution() noexcept;
    ~FineTimerResolution();
    FineTimerResolution(const FineTimerResolution&) = delete;
    FineTimerResolution& operator=(const FineTimerResolution&) = delete;
    FineTimerResolution(FineTimerResolution&&) = delete;
    FineTimerResolution& operator=(FineTimerResolution&&) = delete;
};

// Sleeps until `deadline`, as precisely as the OS allows. On Windows a high-resolution waitable
// timer (Windows 10 1803 and later), which is not bound to the 1 ms timer tick at all: plain
// Sleep woke the arm's 1 kHz loop ~1 ms late on every tick even with a 1 ms tick (journal).
// Elsewhere std::this_thread::sleep_until (clock_nanosleep: already precise).
void sleepUntil(std::chrono::steady_clock::time_point deadline) noexcept;

}  // namespace msc
