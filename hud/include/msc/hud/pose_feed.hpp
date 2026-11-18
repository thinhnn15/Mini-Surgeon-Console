#pragma once

#include "msc/common/latest_value.hpp"
#include "msc/common/spsc_queue.hpp"
#include "msc/hud/pose_history.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace msc::hud {

// The arm's poses on their way from the arm link's thread to the two threads that want them,
// each in the way it needs them:
//
//   link thread ── publish() ──┬── next()   ── render thread: every pose, in order, for the
//                              │                pose history behind the overlay (SpscQueue)
//                              └── newest() ── camera thread: only the newest, to draw the
//                                               instrument into the simulated picture (LatestValue)
//
// One writer, and one reader per side: both hand-overs are lock-free, so the link thread, which
// sends the heartbeats, never waits for a renderer or a camera.
class PoseFeed {
public:
    // At the arm's 1 kHz, 2048 poses are two seconds of a renderer that is not drawing (a
    // minimised window). After that new poses are refused and counted; the history then has a
    // hole, and the overlay hides until fresh poses arrive (REQ-HUD-001).
    explicit PoseFeed(std::size_t queueCapacity = 2048) : queue_(queueCapacity) {}

    // Link thread. Never blocks.
    void publish(const TimedPose& pose) noexcept {
        latest_.publish(Newest{.valid = true, .pose = pose});
        if (!queue_.tryPush(pose)) {
            refused_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Render thread: the next pose in order. False when there is none waiting.
    [[nodiscard]] bool next(TimedPose& out) noexcept { return queue_.tryPop(out); }

    // Camera thread: the newest pose. False if none was ever published.
    [[nodiscard]] bool newest(TimedPose& out) noexcept {
        Newest value;
        static_cast<void>(latest_.read(value));
        if (!value.valid) {
            return false;
        }
        out = value.pose;
        return true;
    }

    // Any thread: poses the queue had no room for.
    [[nodiscard]] std::uint64_t refused() const noexcept { return refused_.load(std::memory_order_relaxed); }

private:
    struct Newest {
        bool valid = false;
        TimedPose pose{};
    };

    SpscQueue<TimedPose> queue_;
    LatestValue<Newest> latest_;
    std::atomic<std::uint64_t> refused_{0};
};

}  // namespace msc::hud
