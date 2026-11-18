#include "msc/hud/tool_camera.hpp"

#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>

namespace msc::hud {
namespace {

constexpr double kAspect = 16.0 / 9.0;

comms::JointArray joints(float j1, float j2, float j3 = 0.0F) {
    comms::JointArray a{};
    a[0] = j1;
    a[1] = j2;
    a[2] = j3;
    return a;
}

double distance(const ScenePoint& a, const ScenePoint& b) {
    return std::sqrt(std::pow(a.x - b.x, 2) + std::pow(a.y - b.y, 2) + std::pow(a.z - b.z, 2));
}

TEST(ToolCamera, AtRestTheTipIsWhereTheConfigurationSaysInTheCentreOfTheImage) {
    const ToolCameraConfig config;
    const ScenePoint tip = toolTip(joints(0.0F, 0.0F), config);
    EXPECT_NEAR(tip.x, config.restTip.x, 1e-12);
    EXPECT_NEAR(tip.y, config.restTip.y, 1e-12);
    EXPECT_NEAR(tip.z, config.restTip.z, 1e-12);

    const ToolView view = viewOfTool(joints(0.0F, 0.0F), kAspect, config);
    ASSERT_TRUE(view.visible);
    EXPECT_TRUE(view.tipInView);
    EXPECT_NEAR(view.tip.x, 0.5, 1e-12);
    EXPECT_NEAR(view.tip.y, 0.5, 1e-12);
    EXPECT_NEAR(view.tipDepth, 0.11, 1e-12);
}

// The instrument pivots about its entry point: swinging it never changes how far the tip is
// from there. Only joint 3 (insertion) does.
TEST(ToolCamera, SwingingKeepsTheTipAtTheSameDistanceFromThePivot) {
    const ToolCameraConfig config;
    const double length = distance(config.restTip, config.pivot);
    for (const float j1 : {-0.4F, -0.1F, 0.3F}) {
        for (const float j2 : {-0.4F, 0.2F, 0.4F}) {
            EXPECT_NEAR(distance(toolTip(joints(j1, j2), config), config.pivot), length, 1e-9)
                << "j1 " << j1 << " j2 " << j2;
        }
    }
    EXPECT_NEAR(distance(toolTip(joints(0.2F, -0.1F, 0.5F), config), config.pivot),
                length + (0.5 * config.insertionPerRadian), 1e-9);
}

// The directions an operator expects: joint 1 moves the tip right, joint 2 moves it up.
TEST(ToolCamera, PositiveJointsMoveTheTipRightAndUpInTheImage) {
    const ToolView rest = viewOfTool(joints(0.0F, 0.0F), kAspect);
    const ToolView right = viewOfTool(joints(0.2F, 0.0F), kAspect);
    const ToolView up = viewOfTool(joints(0.0F, 0.2F), kAspect);
    EXPECT_GT(right.tip.x, rest.tip.x + 0.02);
    EXPECT_LT(up.tip.y, rest.tip.y - 0.02);  // y grows downward
}

// The whole range the hand pad can command (±0.4 rad on joints 1 and 2) stays in the picture,
// so the overlay is never off-screen in normal use.
TEST(ToolCamera, TheHandPadsWholeRangeStaysInsideTheImage) {
    for (int i = -4; i <= 4; ++i) {
        for (int k = -4; k <= 4; ++k) {
            const ToolView view =
                viewOfTool(joints(0.1F * static_cast<float>(i), 0.1F * static_cast<float>(k)), kAspect);
            ASSERT_TRUE(view.visible);
            EXPECT_TRUE(view.tipInView)
                << "j1 " << 0.1 * i << " j2 " << 0.1 * k << " -> " << view.tip.x << ", " << view.tip.y;
            // Clear of the test pattern's barcode strip at the top (1/30 of the height).
            EXPECT_GT(view.tip.y, 0.06);
        }
    }
}

// Perspective: pushed in (further from the camera) the shaft's end looks thinner, and the near
// end of the shaft always looks thicker than the tip.
TEST(ToolCamera, FurtherAwayLooksSmaller) {
    const ToolView rest = viewOfTool(joints(0.0F, 0.0F, 0.0F), kAspect);
    const ToolView pushedIn = viewOfTool(joints(0.0F, 0.0F, 1.0F), kAspect);
    EXPECT_GT(pushedIn.tipDepth, rest.tipDepth);
    EXPECT_LT(pushedIn.tipRadius, rest.tipRadius);
    EXPECT_GT(rest.entryRadius, rest.tipRadius);
    // radius / depth, times the focal length.
    const ToolCameraConfig config;
    EXPECT_NEAR(rest.tipRadius, config.focalLength * config.shaftRadius / 0.11, 1e-12);
}

// The same scene on a 4:3 frame: horizontal positions are the same fraction of the width, and
// the vertical ones stretch by the ratio of the aspect ratios (pixels stay square).
TEST(ToolCamera, AnotherAspectRatioKeepsPixelsSquare) {
    const comms::JointArray pose = joints(0.2F, 0.15F);
    const ToolView wide = viewOfTool(pose, 16.0 / 9.0);
    const ToolView narrow = viewOfTool(pose, 4.0 / 3.0);
    EXPECT_NEAR(wide.tip.x, narrow.tip.x, 1e-12);
    EXPECT_NEAR((wide.tip.y - 0.5) / (16.0 / 9.0), (narrow.tip.y - 0.5) / (4.0 / 3.0), 1e-12);
}

TEST(ToolCamera, ATipBehindTheCameraIsNotVisible) {
    ToolCameraConfig config;
    config.restTip = ScenePoint{.x = 0.0, .y = 0.0, .z = -0.05};
    const ToolView view = viewOfTool(joints(0.0F, 0.0F), kAspect, config);
    EXPECT_FALSE(view.visible);
    EXPECT_FALSE(view.tipInView);
}

// A pivot behind the camera: the visible shaft starts where it comes through the near plane,
// not at a point the projection cannot handle.
TEST(ToolCamera, AShaftThatStartsBehindTheCameraIsCutAtTheNearPlane) {
    ToolCameraConfig config;
    config.pivot = ScenePoint{.x = -0.05, .y = 0.05, .z = -0.02};
    const ToolView view = viewOfTool(joints(0.0F, 0.0F), kAspect, config);
    ASSERT_TRUE(view.visible);
    EXPECT_TRUE(std::isfinite(view.entry.x));
    EXPECT_TRUE(std::isfinite(view.entry.y));
    EXPECT_NEAR(view.entryRadius, config.focalLength * config.shaftRadius / config.nearPlane, 1e-12);
}

TEST(ToolCamera, ATipOutsideTheImageIsVisibleButNotInView) {
    const ToolView view = viewOfTool(joints(0.0F, -0.8F), kAspect);  // swung far down
    EXPECT_TRUE(view.visible);
    EXPECT_FALSE(view.tipInView);
}

}  // namespace
}  // namespace msc::hud
