#include "msc/hud/hud_overlay.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstdint>

namespace msc::hud {
namespace {

using namespace std::chrono_literals;

constexpr double kAspect = 16.0 / 9.0;

TimedPose poseAt(Nanoseconds time, float j1, float j2 = 0.0F) {
    TimedPose p;
    p.time = time;
    p.position[0] = j1;
    p.position[1] = j2;
    return p;
}

// The arm swings joint 1 at 1 rad/s: at time t (seconds) it is at t radians. One pose per
// millisecond from `from` to `to`.
void feedSwing(HudOverlay& overlay, Nanoseconds from, Nanoseconds to) {
    for (Nanoseconds t = from; t <= to; t += 1ms) {
        overlay.addPose(poseAt(t, std::chrono::duration<float>(t).count()));
    }
}

ImagePoint tipFor(float j1, float j2 = 0.0F) {
    comms::JointArray joints{};
    joints[0] = j1;
    joints[1] = j2;
    return viewOfTool(joints, kAspect).tip;
}

double distance(const ImagePoint& a, const ImagePoint& b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

// The point of M5: the frame on screen was captured 80 ms ago, and the marker goes where the
// arm was then, not where it is now.
TEST(HudOverlay, TheMarkerIsWhereTheArmWasWhenTheFrameWasCaptured) {
    HudOverlay overlay;
    feedSwing(overlay, 0ms, 300ms);

    const HudMarker marker = overlay.markerFor(1, 220ms, 300ms, kAspect, HudMode::Synced);
    ASSERT_TRUE(marker.shown());
    EXPECT_LT(distance(marker.centre, tipFor(0.220F)), 1e-6);
    // And clearly not where the arm is now.
    EXPECT_GT(distance(marker.centre, tipFor(0.300F)), 0.02);
    EXPECT_GT(marker.radius, 0.0);
    EXPECT_GT(marker.thickness, 0.0);
}

// The naive overlay, for comparison: the newest pose on an old frame. Wrong by the arm's speed
// times the frame's age.
TEST(HudOverlay, TheNaiveMarkerIsWhereTheArmIsNowWhateverTheFrameShows) {
    HudOverlay overlay;
    feedSwing(overlay, 0ms, 300ms);

    const HudMarker marker = overlay.markerFor(1, 220ms, 300ms, kAspect, HudMode::Naive);
    ASSERT_TRUE(marker.shown());
    EXPECT_LT(distance(marker.centre, tipFor(0.300F)), 1e-6);
}

TEST(HudOverlay, AFrameCapturedBetweenTwoPosesGetsThePoseInBetween) {
    HudOverlay overlay;
    overlay.addPose(poseAt(100ms, 0.10F));
    overlay.addPose(poseAt(110ms, 0.20F));

    const HudMarker marker = overlay.markerFor(1, 105ms, 120ms, kAspect, HudMode::Synced);
    ASSERT_TRUE(marker.shown());
    EXPECT_LT(distance(marker.centre, tipFor(0.15F)), 1e-6);
}

TEST(HudOverlay, REQ_HUD_001_NoMarkerBeforeAnyPoseHasArrived) {
    HudOverlay overlay;
    EXPECT_EQ(overlay.markerFor(1, 10ms, 20ms, kAspect, HudMode::Synced).state, HudState::NoData);
    EXPECT_EQ(overlay.markerFor(1, 10ms, 20ms, kAspect, HudMode::Naive).state, HudState::NoData);
}

// REQ-HUD-001: the arm link goes quiet at 100 ms. Frames captured up to then still get their
// marker (the arm's pose at their moment is known); frames captured after it get none, instead
// of a marker left where the arm last reported.
TEST(HudOverlay, REQ_HUD_001_FramesCapturedAfterTheLastPoseGetNoMarker) {
    HudOverlay overlay;
    feedSwing(overlay, 0ms, 100ms);

    EXPECT_TRUE(overlay.markerFor(1, 95ms, 180ms, kAspect, HudMode::Synced).shown());
    EXPECT_TRUE(overlay.markerFor(2, 100ms, 190ms, kAspect, HudMode::Synced).shown());
    const HudMarker after = overlay.markerFor(3, 101ms, 200ms, kAspect, HudMode::Synced);
    EXPECT_FALSE(after.shown());
    EXPECT_EQ(after.state, HudState::Stale);
}

// REQ-HUD-001 for the naive overlay: its pose is "the newest", so it is stale when the newest
// is older than the limit.
TEST(HudOverlay, REQ_HUD_001_TheNaiveMarkerDisappearsWhenTheNewestPoseIsTooOld) {
    HudOverlay overlay;  // limit 30 ms
    feedSwing(overlay, 0ms, 100ms);

    EXPECT_TRUE(overlay.markerFor(1, 50ms, 130ms, kAspect, HudMode::Naive).shown());
    EXPECT_EQ(overlay.markerFor(1, 50ms, 130ms + 1ns, kAspect, HudMode::Naive).state, HudState::Stale);
}

// REQ-HUD-001: the link was down from 100 to 300 ms. A frame captured in the hole gets no
// marker; frames on either side of it do.
TEST(HudOverlay, REQ_HUD_001_AFrameCapturedDuringALinkOutageGetsNoMarker) {
    HudOverlay overlay;
    feedSwing(overlay, 0ms, 100ms);
    feedSwing(overlay, 300ms, 400ms);

    EXPECT_TRUE(overlay.markerFor(1, 90ms, 400ms, kAspect, HudMode::Synced).shown());
    EXPECT_EQ(overlay.markerFor(2, 200ms, 400ms, kAspect, HudMode::Synced).state, HudState::Stale);
    EXPECT_TRUE(overlay.markerFor(3, 350ms, 400ms, kAspect, HudMode::Synced).shown());
}

// The telemetry for a frame's moment can arrive after the frame is first drawn (a video path
// faster than the arm link). "Not known" is asked again on the next draw, not remembered.
TEST(HudOverlay, AFrameWhoseTimeIsNotKnownYetIsAskedAgainOnTheNextDraw) {
    HudOverlay overlay;
    feedSwing(overlay, 0ms, 100ms);

    EXPECT_EQ(overlay.markerFor(7, 105ms, 106ms, kAspect, HudMode::Synced).state, HudState::Stale);
    feedSwing(overlay, 101ms, 110ms);  // the missing telemetry arrives
    const HudMarker marker = overlay.markerFor(7, 105ms, 112ms, kAspect, HudMode::Synced);
    ASSERT_TRUE(marker.shown());
    EXPECT_LT(distance(marker.centre, tipFor(0.105F)), 1e-6);
}

// A frozen picture: the same frame stays on screen while the history moves on and forgets its
// moment. The marker that belongs to that frame stays with it: it is as true as the picture.
TEST(HudOverlay, AFrameThatStaysOnScreenKeepsItsMarker) {
    HudOverlay overlay({.capacity = 64, .maxGap = 30ms});
    feedSwing(overlay, 0ms, 50ms);
    const HudMarker first = overlay.markerFor(9, 40ms, 50ms, kAspect, HudMode::Synced);
    ASSERT_TRUE(first.shown());

    feedSwing(overlay, 51ms, 500ms);  // 64 poses kept: 40 ms is long gone
    ASSERT_EQ(overlay.history().at(40ms).result, PoseLookup::BeforeHistory);
    const HudMarker later = overlay.markerFor(9, 40ms, 500ms, kAspect, HudMode::Synced);
    ASSERT_TRUE(later.shown());
    EXPECT_DOUBLE_EQ(later.centre.x, first.centre.x);
    EXPECT_DOUBLE_EQ(later.centre.y, first.centre.y);

    // Another frame from that forgotten time has nothing to go by.
    EXPECT_EQ(overlay.markerFor(10, 41ms, 500ms, kAspect, HudMode::Synced).state, HudState::Stale);
    // And after a change of source the frame numbers mean nothing.
    overlay.forgetFrame();
    EXPECT_EQ(overlay.markerFor(9, 40ms, 500ms, kAspect, HudMode::Synced).state, HudState::Stale);
}

TEST(HudOverlay, SwitchedOffDrawsNothingAndSaysSo) {
    HudOverlay overlay;
    feedSwing(overlay, 0ms, 100ms);
    const HudMarker marker = overlay.markerFor(1, 50ms, 100ms, kAspect, HudMode::Off);
    EXPECT_FALSE(marker.shown());
    EXPECT_EQ(marker.state, HudState::Off);
}

// The pose is known but the tip has left the picture: no marker at the edge pretending.
TEST(HudOverlay, ATipOutsideThePictureGetsNoMarker) {
    HudOverlay overlay;
    overlay.addPose(poseAt(10ms, 0.0F, -0.8F));
    overlay.addPose(poseAt(20ms, 0.0F, -0.8F));
    EXPECT_EQ(overlay.markerFor(1, 15ms, 20ms, kAspect, HudMode::Synced).state, HudState::OutOfView);
}

// The ring is wider than the tip it surrounds, so it never covers the instrument.
TEST(HudOverlay, TheRingLeavesTheTipUncovered) {
    HudOverlay overlay;
    feedSwing(overlay, 0ms, 100ms);
    const HudMarker marker = overlay.markerFor(1, 50ms, 100ms, kAspect, HudMode::Synced);
    ASSERT_TRUE(marker.shown());
    comms::JointArray joints{};
    joints[0] = 0.05F;
    const double tipRadius = viewOfTool(joints, kAspect).tipRadius;
    EXPECT_GT(marker.radius - (marker.thickness / 2.0), 2.0 * tipRadius);
}

TEST(HudOverlay, EveryStateHasAText) {
    for (const HudState state :
         {HudState::Shown, HudState::Off, HudState::NoData, HudState::Stale, HudState::OutOfView}) {
        EXPECT_STRNE(toString(state), "unknown");
    }
}

}  // namespace
}  // namespace msc::hud
