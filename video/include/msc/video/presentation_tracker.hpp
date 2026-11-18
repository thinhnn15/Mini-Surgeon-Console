#pragma once

#include "msc/common/clock.hpp"
#include "msc/common/latency_stats.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace msc::video {

// What the console knows about how frames reach the screen (REQ-VID-002).
struct PresentationStats {
    // Latency percentiles over the tracker's rolling window. Empty until a frame is presented.
    std::size_t samples = 0;
    std::optional<Nanoseconds> captureToPresentP50;
    std::optional<Nanoseconds> captureToPresentP99;
    std::optional<Nanoseconds> captureToPresentMax;
    // The first leg: from capture until the renderer picked the frame up and uploaded it.
    // Shows how much of the latency is waiting for the next draw, as opposed to drawing it.
    std::optional<Nanoseconds> captureToUploadP50;
    std::optional<Nanoseconds> captureToUploadP99;

    // Rates over the time since the previous takeStats() call.
    double renderFps = 0.0;  // swaps per second (how often the window presents)
    double videoFps = 0.0;   // new video frames presented per second

    // Totals since construction or reset().
    std::uint64_t swaps = 0;
    std::uint64_t framesShown = 0;    // frames that reached a swap
    std::uint64_t framesDropped = 0;  // frames the source made that were never picked up
    std::optional<Nanoseconds> firstSwap;
    std::optional<Nanoseconds> lastSwap;
};

// Follows frames along the display path and turns the timestamps into PresentationStats.
// No Qt and no clock of its own: the caller passes every time in, so tests use plain numbers.
//
// The path and its timestamps (all on the same monotonic clock as Frame::captureTime):
//   capture  - Frame::captureTime, set by the source when the image was taken
//   upload   - the renderer took the frame and issued its texture upload
//   present  - the window's swap (Present) for the frame that drew it has returned
// "Present" is when the frame was handed to the OS for display, not when light leaves the
// screen: the compositor, the wait for scan-out and the panel itself come after. That
// remainder needs a camera pointed at the screen (glass-to-glass, M2.5).
//
// One thread (the render thread).
class PresentationTracker {
public:
    // window: how many recent frames the latency percentiles cover (600 = 10 s at 60 fps).
    explicit PresentationTracker(std::size_t window = 600);

    // The renderer took a new frame and uploaded it; it will be drawn in the current frame.
    // A gap in sequence numbers counts as dropped frames: the source made them, but the
    // renderer only ever takes the newest (REQ-VID-001), so these were never shown.
    void frameUploaded(std::uint64_t sequence, Nanoseconds captureTime, Nanoseconds uploadTime);

    // The window presented (any swap, with or without a new video frame in it).
    void swapped(Nanoseconds presentTime);

    // Current statistics. Also restarts the interval the FPS figures are measured over.
    [[nodiscard]] PresentationStats takeStats(Nanoseconds now);

    // Forget everything (for example after a warm-up period).
    void reset();

private:
    struct Pending {
        Nanoseconds captureTime;
        Nanoseconds uploadTime;
    };

    LatencyStats captureToPresent_;
    LatencyStats captureToUpload_;
    std::optional<Pending> pending_;  // uploaded, waiting for its swap
    std::uint64_t lastSequence_ = 0;

    std::uint64_t swaps_ = 0;
    std::uint64_t shown_ = 0;
    std::uint64_t dropped_ = 0;
    std::optional<Nanoseconds> firstSwap_;
    std::optional<Nanoseconds> lastSwap_;

    // For the FPS figures: counts and time at the previous takeStats().
    std::optional<Nanoseconds> intervalStart_;
    std::uint64_t swapsAtIntervalStart_ = 0;
    std::uint64_t shownAtIntervalStart_ = 0;
};

}  // namespace msc::video
