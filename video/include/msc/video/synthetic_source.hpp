#pragma once

#include "msc/common/clock.hpp"
#include "msc/common/triple_buffer.hpp"
#include "msc/video/frame.hpp"
#include "msc/video/frame_source.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <stop_token>
#include <thread>
#include <vector>

namespace msc::video {

struct SyntheticSourceConfig {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    Nanoseconds period{16'666'667};  // 60 fps
    // How long a frame takes from capture to delivery (M5.2). A real camera chain (exposure,
    // read-out, processing, transport) takes 50 to 100 ms; the pattern alone takes none, which
    // would hide every mistake in how the overlay is timed. The delay is rounded up to whole
    // periods: a frame is delivered that many ticks after the tick that captured it.
    // captureTime itself is not changed: it still says when the picture was taken.
    Nanoseconds delay{0};
    // Called on the producer thread for every frame, after the pattern is drawn and
    // captureTime is set: draws what else the camera sees (the instrument). Must not block.
    std::function<void(Frame&)> scene{};
};

// A video source that needs no hardware: renders the test pattern on its own thread at a
// fixed rate and hands frames over in place through a TripleBuffer (ADR 0002). Lets the
// whole video path run on CI and in tests, the same way every time.
//
// Two clocks, on purpose:
//   - The injected Clock stamps captureTime ("what time is it"), so tests can control it.
//   - Pacing (how long to sleep) uses std::chrono::steady_clock directly, because the
//     thread really has to wait; a ManualClock cannot make a thread sleep.
class SyntheticSource final : public FrameSource {
public:
    SyntheticSource(const Clock& clock, SyntheticSourceConfig config);
    ~SyntheticSource() override;

    SyntheticSource(const SyntheticSource&) = delete;
    SyntheticSource& operator=(const SyntheticSource&) = delete;
    SyntheticSource(SyntheticSource&&) = delete;
    SyntheticSource& operator=(SyntheticSource&&) = delete;

    void start() override;
    void stop() override;
    [[nodiscard]] bool acquireLatest() noexcept override;
    [[nodiscard]] const Frame& latest() const noexcept override;

    // One tick, on the calling thread: captures a frame right now and delivers one. With no
    // delay that is the frame just captured; with a delay of N periods, the frame captured N
    // ticks ago (nothing during the first N ticks). This is the whole per-tick work of the
    // producer thread; it is public so tests can drive the source step by step without a
    // thread. Do not call it while the source is started.
    void produceOne();

    // Any thread. For tests and statistics.
    [[nodiscard]] std::uint64_t framesProduced() const noexcept {
        return produced_.load(std::memory_order_relaxed);
    }
    // Times the producer fell more than one period behind and skipped ahead instead of
    // bursting to catch up.
    [[nodiscard]] std::uint64_t overruns() const noexcept {
        return overruns_.load(std::memory_order_relaxed);
    }
    // The delay in whole periods: how many ticks a frame waits before it is delivered.
    [[nodiscard]] std::size_t delayTicks() const noexcept { return waiting_.size(); }

private:
    void run(const std::stop_token& stop);
    void capture(Frame& frame);

    const Clock* clock_;  // not owned; outlives the source
    SyntheticSourceConfig config_;
    TripleBuffer<Frame> buffer_;
    std::uint64_t nextSequence_ = 1;  // writer side only
    // The delay line (writer side only): the frames captured in the last N ticks, in a ring of
    // N frames allocated up front; empty when the delay is zero. Each tick, the slot that holds
    // the frame captured N ticks ago is swapped with the triple buffer's write slot (the frame
    // is delivered without copying its pixels) and the next frame is captured into the slot.
    std::vector<Frame> waiting_;
    std::size_t waitingNext_ = 0;   // the slot this tick delivers from and captures into
    std::size_t waitingCount_ = 0;  // frames captured and not yet delivered, at most N
    std::atomic<std::uint64_t> produced_{0};
    std::atomic<std::uint64_t> overruns_{0};
    // Last member, so that even without the explicit stop() in the destructor the thread
    // would be stopped and joined before the buffer it writes to is destroyed.
    std::jthread thread_;
};

}  // namespace msc::video
