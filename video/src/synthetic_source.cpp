#include "msc/video/synthetic_source.hpp"

#include "msc/video/test_pattern.hpp"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
// windows.h first: timeapi.h depends on it.
#include <timeapi.h>
#endif

namespace msc::video {

namespace {

// Windows wakes sleeping threads on a ~15.6 ms timer tick by default, so a 16.7 ms frame
// period comes out as 15.6 or 31.2 ms: measured gaps 15.0-32.2 ms at 60 fps, i.e. visibly
// doubled frames. With a 1 ms tick the gaps were 15.0-18.6 ms. The request is per process
// on Windows 10 2004 and later, and is held only while the producer thread runs.
class FineTimerResolution {
public:
#if defined(_WIN32)
    FineTimerResolution() noexcept { timeBeginPeriod(1); }
    ~FineTimerResolution() { timeEndPeriod(1); }
#else
    FineTimerResolution() noexcept = default;  // Linux timers are already fine-grained
    ~FineTimerResolution() = default;
#endif
    FineTimerResolution(const FineTimerResolution&) = delete;
    FineTimerResolution& operator=(const FineTimerResolution&) = delete;
    FineTimerResolution(FineTimerResolution&&) = delete;
    FineTimerResolution& operator=(FineTimerResolution&&) = delete;
};

// All three slots start as fully allocated frames, so the producer never allocates.
// The sequence is reset to 0 so that latest() reads as "no frame yet" before the first one.
Frame makeInitialFrame(const SyntheticSourceConfig& config) {
    if (config.width < kMinPatternWidth || config.height < kMinPatternHeight) {
        throw std::invalid_argument("SyntheticSource: frame size below the test pattern minimum");
    }
    if (config.period <= Nanoseconds::zero()) {
        throw std::invalid_argument("SyntheticSource: period must be positive");
    }
    return Frame::allocate(config.width, config.height);
}

}  // namespace

SyntheticSource::SyntheticSource(const Clock& clock, SyntheticSourceConfig config)
    : clock_(&clock), config_(config), buffer_(makeInitialFrame(config)) {}

SyntheticSource::~SyntheticSource() {
    stop();
}

void SyntheticSource::start() {
    if (thread_.joinable()) {
        return;
    }
    thread_ = std::jthread([this](const std::stop_token& stop) { run(stop); });
}

void SyntheticSource::stop() {
    if (!thread_.joinable()) {
        return;
    }
    thread_.request_stop();
    thread_.join();
}

bool SyntheticSource::acquireLatest() noexcept {
    return buffer_.acquireLatest();
}

const Frame& SyntheticSource::latest() const noexcept {
    return buffer_.readSlot();
}

void SyntheticSource::produceOne() {
    Frame& frame = buffer_.writeSlot();
    frame.captureTime = clock_->now();
    renderTestPattern(frame, nextSequence_);
    ++nextSequence_;
    buffer_.publish();
    produced_.fetch_add(1, std::memory_order_relaxed);
}

void SyntheticSource::run(const std::stop_token& stop) {
    // Wait with a condition variable rather than sleep_until: condition_variable_any's
    // stop_token overload wakes up as soon as stop is requested, so stop() returns at once
    // instead of after up to one full period. Nothing else is shared through this mutex.
    std::mutex mutex;
    std::condition_variable_any wake;
    const FineTimerResolution fineTimer;

    auto next = std::chrono::steady_clock::now();
    while (!stop.stop_requested()) {
        produceOne();

        // Deadline-based pacing: next += period, not "sleep for one period". Sleeping for a
        // fixed time after the work would add the work time (and every wake-up delay) to
        // each frame, and the rate would drift below 60 fps.
        next += config_.period;
        const auto now = std::chrono::steady_clock::now();
        if (now > next + config_.period) {
            // More than a whole frame late (debugger, overloaded machine). Do not produce a
            // burst of frames to catch up; a camera would just deliver the next one late.
            overruns_.fetch_add(1, std::memory_order_relaxed);
            next = now;
        }

        std::unique_lock lock(mutex);
        // Returns at the deadline, or early when stop is requested. The predicate is never
        // true: the only reasons to wake are those two.
        static_cast<void>(wake.wait_until(lock, stop, next, [] { return false; }));
    }
}

}  // namespace msc::video
