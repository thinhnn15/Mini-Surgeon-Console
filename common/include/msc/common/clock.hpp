#pragma once

#include <chrono>

namespace msc {

using Nanoseconds = std::chrono::nanoseconds;

// A monotonic time source. Code that needs the time takes a Clock& instead of
// calling steady_clock directly, so timeouts and watchdogs can be tested with
// ManualClock: the test moves time forward, nothing sleeps, nothing is flaky.
class Clock {
public:
    Clock() = default;
    Clock(const Clock&) = delete;
    Clock& operator=(const Clock&) = delete;
    Clock(Clock&&) = delete;
    Clock& operator=(Clock&&) = delete;
    virtual ~Clock() = default;

    [[nodiscard]] virtual Nanoseconds now() const noexcept = 0;
};

// Never jumps (unlike system_clock, which NTP or the user can move).
class SteadyClock final : public Clock {
public:
    [[nodiscard]] Nanoseconds now() const noexcept override {
        return std::chrono::duration_cast<Nanoseconds>(std::chrono::steady_clock::now().time_since_epoch());
    }
};

// For tests: time only moves when the test says so.
class ManualClock final : public Clock {
public:
    [[nodiscard]] Nanoseconds now() const noexcept override { return now_; }
    void advance(Nanoseconds by) noexcept { now_ += by; }

private:
    Nanoseconds now_{0};
};

}  // namespace msc
