#pragma once

#include "msc/common/clock.hpp"
#include "msc/common/triple_buffer.hpp"
#include "msc/video/frame.hpp"
#include "msc/video/frame_source.hpp"

#include <atomic>
#include <cstdint>
#include <stop_token>
#include <thread>

namespace msc::video {

struct SyntheticSourceConfig {
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    Nanoseconds period{16'666'667};  // 60 fps
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

    // Renders and publishes one frame right now, on the calling thread. This is the whole
    // per-frame work of the producer thread; it is public so tests can drive the source
    // step by step without a thread. Do not call it while the source is started.
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

private:
    void run(const std::stop_token& stop);

    const Clock* clock_;  // not owned; outlives the source
    SyntheticSourceConfig config_;
    TripleBuffer<Frame> buffer_;
    std::uint64_t nextSequence_ = 1;  // writer side only
    std::atomic<std::uint64_t> produced_{0};
    std::atomic<std::uint64_t> overruns_{0};
    // Last member, so that even without the explicit stop() in the destructor the thread
    // would be stopped and joined before the buffer it writes to is destroyed.
    std::jthread thread_;
};

}  // namespace msc::video
