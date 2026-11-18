#include "msc/video/synthetic_source.hpp"

#include "msc/common/fine_timer.hpp"
#include "msc/video/test_pattern.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace msc::video {

namespace {

// All three slots start as fully allocated frames, so the producer never allocates.
// The sequence is reset to 0 so that latest() reads as "no frame yet" before the first one.
Frame makeInitialFrame(const SyntheticSourceConfig& config) {
    if (config.width < kMinPatternWidth || config.height < kMinPatternHeight) {
        throw std::invalid_argument("SyntheticSource: frame size below the test pattern minimum");
    }
    if (config.period <= Nanoseconds::zero()) {
        throw std::invalid_argument("SyntheticSource: period must be positive");
    }
    if (config.delay < Nanoseconds::zero()) {
        throw std::invalid_argument("SyntheticSource: delay must not be negative");
    }
    return Frame::allocate(config.width, config.height);
}

// The delay in whole periods, rounded up: a frame is never delivered early.
std::size_t delayTicksFor(const SyntheticSourceConfig& config) {
    return static_cast<std::size_t>((config.delay + config.period - Nanoseconds{1}) / config.period);
}

}  // namespace

SyntheticSource::SyntheticSource(const Clock& clock, SyntheticSourceConfig config)
    : clock_(&clock),
      config_(std::move(config)),
      buffer_(makeInitialFrame(config_)),
      waiting_(delayTicksFor(config_), Frame::allocate(config_.width, config_.height)) {}

SyntheticSource::~SyntheticSource() {
    stop();
}

void SyntheticSource::start() {
    if (thread_.joinable()) {
        return;
    }
    // Frames still waiting from before a stop are older than the delay by now: a camera that
    // was switched off does not deliver what it saw before.
    waitingNext_ = 0;
    waitingCount_ = 0;
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

// The picture as it is at this instant: the time, the pattern, and whatever else is in view.
void SyntheticSource::capture(Frame& frame) {
    frame.captureTime = clock_->now();
    renderTestPattern(frame, nextSequence_);
    ++nextSequence_;
    if (config_.scene) {
        config_.scene(frame);
    }
}

void SyntheticSource::produceOne() {
    if (waiting_.empty()) {
        capture(buffer_.writeSlot());  // no delay: straight into the mailbox
        buffer_.publish();
        produced_.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    // Delivery goes by ticks, not by comparing times. "Deliver what is at least 80 ms old" at
    // 60 fps means 5 ticks = 83.3 ms, with only 3.3 ms to spare: a tick that came 4 ms late
    // made the frame captured there miss its turn, and the next tick delivered two at once
    // (one shown, one dropped). Measured with the first version: 41 frames dropped in 6 s.
    Frame& slot = waiting_[waitingNext_];
    if (waitingCount_ == waiting_.size()) {
        // The slot holds the frame captured N ticks ago: its turn. Swap, not copy: it goes
        // into the mailbox's write slot, and the frame that was there becomes this slot.
        std::swap(slot, buffer_.writeSlot());
        buffer_.publish();
        produced_.fetch_add(1, std::memory_order_relaxed);
        --waitingCount_;
    }
    capture(slot);
    ++waitingCount_;
    waitingNext_ = (waitingNext_ + 1) % waiting_.size();
}

void SyntheticSource::run(const std::stop_token& stop) {
    // Wait with a condition variable rather than sleep_until: condition_variable_any's
    // stop_token overload wakes up as soon as stop is requested, so stop() returns at once
    // instead of after up to one full period. Nothing else is shared through this mutex.
    std::mutex mutex;
    std::condition_variable_any wake;
    // Held for its constructor/destructor only. On Linux both are empty, so GCC would call
    // it unused without the attribute.
    [[maybe_unused]] const msc::FineTimerResolution fineTimer;

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
