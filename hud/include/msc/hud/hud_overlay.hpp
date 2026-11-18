#pragma once

#include "msc/common/clock.hpp"
#include "msc/hud/pose_history.hpp"
#include "msc/hud/tool_camera.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace msc::hud {

// How the marker's position is chosen.
enum class HudMode : std::uint8_t {
    Synced,  // the pose at the time the frame on screen was captured (the point of M5)
    Naive,   // the newest pose, whatever the frame's age: wrong by speed x latency. For comparison.
    Off,
};

// What the overlay did for the frame on screen, and why.
enum class HudState : std::uint8_t {
    Shown,      // the marker is drawn
    Off,        // switched off
    NoData,     // no pose has ever arrived
    Stale,      // REQ-HUD-001: the pose for this frame's time is not known, so nothing is drawn
    OutOfView,  // the pose is known, and the instrument's tip is outside the picture
};

[[nodiscard]] const char* toString(HudState state) noexcept;

// The ring drawn around the instrument's tip. Position and size are fractions of the image
// (see ImagePoint), so they hold for any window size.
struct HudMarker {
    HudState state = HudState::NoData;
    ImagePoint centre{};
    double radius = 0.0;     // of the ring's middle line, as a fraction of the image width
    double thickness = 0.0;  // of the ring's line, same unit

    [[nodiscard]] bool shown() const noexcept { return state == HudState::Shown; }
};

// The ring is this many times the shaft's apparent radius at the tip: wide enough to leave the
// tip itself uncovered, so the instrument is never hidden by its own marker.
inline constexpr double kHudRingScale = 3.8;
inline constexpr double kHudRingThicknessScale = 0.35;

// The ring's colour, RGB: cyan when synchronised, yellow for the naive mode, so it is never
// mistaken for the real thing. Neither colour appears in the test pattern or on the
// instrument, so the ring can be found again in a picture of the screen (M5.4).
inline constexpr std::array<std::uint8_t, 3> kHudSyncedColour{0, 229, 255};
inline constexpr std::array<std::uint8_t, 3> kHudNaiveColour{255, 214, 0};

// Decides, for each frame on screen, where the marker goes or that there is none (M5.3).
//
// Synced mode asks the pose history for the pose at the frame's capture time. The answer for
// one frame never changes once it is known (the past does not change), so it is kept for as
// long as that frame stays on screen: a frozen picture keeps the marker that belongs to it.
// While the answer is "not known" it is asked again on every draw, because the telemetry for
// that moment may simply not have arrived yet.
//
// Not thread safe: owned by the render thread.
class HudOverlay {
public:
    explicit HudOverlay(PoseHistoryConfig history = {}, ToolCameraConfig camera = {});

    // A pose from the arm link, in time order.
    void addPose(const TimedPose& pose) noexcept { static_cast<void>(history_.push(pose)); }

    // The marker for the frame on screen. `frameSequence` and `captureTime` are the frame's;
    // `now` is the time of this draw (only Naive mode uses it, to tell how old the newest pose
    // is); `aspect` is the frame's width divided by its height.
    [[nodiscard]] HudMarker markerFor(std::uint64_t frameSequence, Nanoseconds captureTime, Nanoseconds now,
                                      double aspect, HudMode mode) noexcept;

    // A new video source: frame numbers start again, so nothing remembered applies.
    void forgetFrame() noexcept { remembered_.reset(); }

    [[nodiscard]] const PoseHistory& history() const noexcept { return history_; }

private:
    struct Remembered {
        std::uint64_t frameSequence = 0;
        double aspect = 0.0;
        HudMarker marker{};
    };

    [[nodiscard]] HudMarker markerAt(const comms::JointArray& pose, double aspect) const noexcept;

    PoseHistoryConfig historyConfig_;
    ToolCameraConfig camera_;
    PoseHistory history_;
    std::optional<Remembered> remembered_;  // Synced mode only
};

}  // namespace msc::hud
