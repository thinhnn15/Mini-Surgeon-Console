#pragma once

#include "msc/common/clock.hpp"

#include <chrono>
#include <cstdint>
#include <optional>

namespace msc::video {

// Whether the picture on the screen is live (REQ-VID-003).
//
// The danger is not a black screen, which everybody notices. It is a frozen one: the camera
// is unplugged or its driver stalls, the renderer keeps drawing the last frame it has, and
// that frame looks exactly like live video. Nothing about the picture itself says it is old.
// So the console watches the frame's timestamp: the capture time of the newest frame that
// reached the screen. If that gets old, the view is not live, whatever caused it (source,
// driver, cable, renderer).
enum class VideoSignal : std::uint8_t {
    Waiting,  // no frame has been shown yet (starting, or a camera that never delivers)
    Live,     // the newest frame on screen is recent
    Frozen,   // frames stopped: the picture on screen is old
};

// How old the newest frame may get before the view counts as frozen. A webcam in poor light
// can drop to 7.5 fps (133 ms per frame), and frames arrive with some jitter: 250 ms is about
// two of those frame periods. Shorter would flag a slow camera as frozen; much longer would
// let a surgeon act on an old picture for that long.
inline constexpr Nanoseconds kFrozenAfter = std::chrono::milliseconds(250);

// newestCapture: Frame::captureTime of the newest frame that reached the screen, or nothing
// if none has. now: on the same clock.
[[nodiscard]] constexpr VideoSignal classifySignal(std::optional<Nanoseconds> newestCapture, Nanoseconds now,
                                                   Nanoseconds frozenAfter = kFrozenAfter) noexcept {
    if (!newestCapture) {
        return VideoSignal::Waiting;
    }
    return now - *newestCapture > frozenAfter ? VideoSignal::Frozen : VideoSignal::Live;
}

}  // namespace msc::video
