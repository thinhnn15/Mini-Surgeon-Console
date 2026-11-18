#include "msc/hud/hud_overlay.hpp"

namespace msc::hud {

const char* toString(HudState state) noexcept {
    switch (state) {
        case HudState::Shown:
            return "shown";
        case HudState::Off:
            return "off";
        case HudState::NoData:
            return "no arm data yet";
        case HudState::Stale:
            return "no arm data for the picture on screen";
        case HudState::OutOfView:
            return "instrument outside the picture";
    }
    return "unknown";
}

HudOverlay::HudOverlay(PoseHistoryConfig history, ToolCameraConfig camera)
    : historyConfig_(history), camera_(camera), history_(history) {}

HudMarker HudOverlay::markerAt(const comms::JointArray& pose, double aspect) const noexcept {
    const ToolView view = viewOfTool(pose, aspect, camera_);
    if (!view.tipInView) {
        return HudMarker{.state = HudState::OutOfView, .centre = {}, .radius = 0.0, .thickness = 0.0};
    }
    return HudMarker{.state = HudState::Shown,
                     .centre = view.tip,
                     .radius = view.tipRadius * kHudRingScale,
                     .thickness = view.tipRadius * kHudRingThicknessScale};
}

HudMarker HudOverlay::markerFor(std::uint64_t frameSequence, Nanoseconds captureTime, Nanoseconds now,
                                double aspect, HudMode mode) noexcept {
    if (mode == HudMode::Off) {
        return HudMarker{.state = HudState::Off, .centre = {}, .radius = 0.0, .thickness = 0.0};
    }
    if (history_.size() == 0) {
        return HudMarker{.state = HudState::NoData, .centre = {}, .radius = 0.0, .thickness = 0.0};
    }

    if (mode == HudMode::Naive) {
        // The newest pose, on whatever frame is on screen. REQ-HUD-001 still holds: a pose
        // older than the gap limit (the arm link went quiet) is not drawn.
        const TimedPose newest = *history_.newest();
        if (now - newest.time > historyConfig_.maxGap) {
            return HudMarker{.state = HudState::Stale, .centre = {}, .radius = 0.0, .thickness = 0.0};
        }
        return markerAt(newest.position, aspect);
    }

    // Synced. The answer for this frame, if it is already known, cannot change.
    if (remembered_ && remembered_->frameSequence == frameSequence && remembered_->aspect == aspect) {
        return remembered_->marker;
    }
    const PoseAt pose = history_.at(captureTime);
    if (!pose.found()) {
        // REQ-HUD-001. Not remembered: the pose for this moment may still arrive.
        return HudMarker{.state = HudState::Stale, .centre = {}, .radius = 0.0, .thickness = 0.0};
    }
    const HudMarker marker = markerAt(pose.position, aspect);
    remembered_ = Remembered{.frameSequence = frameSequence, .aspect = aspect, .marker = marker};
    return marker;
}

}  // namespace msc::hud
