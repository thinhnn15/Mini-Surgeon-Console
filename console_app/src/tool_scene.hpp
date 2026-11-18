#pragma once

#include "msc/hud/pose_feed.hpp"
#include "msc/hud/tool_camera.hpp"
#include "msc/video/frame.hpp"
#include "msc/video/scene.hpp"
#include "msc/video/test_pattern.hpp"

#include <functional>
#include <memory>
#include <utility>

// What the simulated camera sees besides the test pattern (M5.2): the instrument, at the arm's
// newest pose at the moment the frame is captured. Runs on the synthetic source's thread.
//
// The camera learns the pose from the arm's telemetry, because the arm is another process and
// there is no other way to know. A real camera would see the real arm whatever the link does;
// this one, with the link cut, keeps drawing the last pose it heard. That is a limit of the
// simulation, not of the console.
inline std::function<void(msc::video::Frame&)> makeToolScene(std::shared_ptr<msc::hud::PoseFeed> poses) {
    return [poses = std::move(poses)](msc::video::Frame& frame) {
        msc::hud::TimedPose pose;
        if (frame.empty() || !poses->newest(pose)) {
            return;  // no arm heard yet: an empty scene
        }
        const auto width = static_cast<double>(frame.width);
        const auto height = static_cast<double>(frame.height);
        const msc::hud::ToolView view = msc::hud::viewOfTool(pose.position, width / height);
        if (!view.visible) {
            return;
        }
        msc::video::drawTool(
            frame,
            msc::video::ToolSprite{.entry = {.x = view.entry.x * width, .y = view.entry.y * height},
                                   .tip = {.x = view.tip.x * width, .y = view.tip.y * height},
                                   .entryRadius = view.entryRadius * width,
                                   .tipRadius = view.tipRadius * width},
            msc::video::barcodeRows(frame.height));
    };
}
