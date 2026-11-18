#include "msc/hud/pose_history.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <stdexcept>

namespace msc::hud {
namespace {

using namespace std::chrono_literals;

// A pose with every joint at `value`: enough to see which sample (or mix) came back.
TimedPose pose(Nanoseconds time, float value) {
    TimedPose p;
    p.time = time;
    p.position.fill(value);
    return p;
}

// One sample per millisecond from `from` to `to` (inclusive), each joint at its time in
// seconds: an arm moving at 1 rad/s.
void fillEveryMillisecond(PoseHistory& history, Nanoseconds from, Nanoseconds to) {
    for (Nanoseconds t = from; t <= to; t += 1ms) {
        ASSERT_TRUE(history.push(pose(t, std::chrono::duration<float>(t).count())));
    }
}

TEST(PoseHistory, RejectsAConfigurationThatCannotWork) {
    EXPECT_THROW(PoseHistory({.capacity = 1, .maxGap = 30ms}), std::invalid_argument);
    EXPECT_THROW(PoseHistory({.capacity = 16, .maxGap = 0ms}), std::invalid_argument);
}

TEST(PoseHistory, REQ_HUD_001_AnEmptyHistoryKnowsNothing) {
    const PoseHistory history;
    const PoseAt at = history.at(100ms);
    EXPECT_FALSE(at.found());
    EXPECT_EQ(at.result, PoseLookup::NoData);
    EXPECT_FALSE(history.newest().has_value());
    EXPECT_FALSE(history.oldest().has_value());
}

TEST(PoseHistory, AnExactSampleComesBackAsItIs) {
    PoseHistory history;
    ASSERT_TRUE(history.push(pose(10ms, 0.25F)));
    ASSERT_TRUE(history.push(pose(11ms, 0.50F)));

    const PoseAt first = history.at(10ms);
    ASSERT_TRUE(first.found());
    EXPECT_FLOAT_EQ(first.position[0], 0.25F);
    const PoseAt second = history.at(11ms);
    ASSERT_TRUE(second.found());
    EXPECT_FLOAT_EQ(second.position[5], 0.50F);
}

// The reason the history exists: a frame captured between two samples gets the pose of its
// own moment, a straight line between the two.
TEST(PoseHistory, InterpolatesBetweenTheTwoSamplesAroundTheTime) {
    PoseHistory history;
    ASSERT_TRUE(history.push(pose(100ms, 1.0F)));
    ASSERT_TRUE(history.push(pose(110ms, 2.0F)));

    const PoseAt quarter = history.at(102500us);
    ASSERT_TRUE(quarter.found());
    for (const float joint : quarter.position) {
        EXPECT_NEAR(joint, 1.25F, 1e-6F);
    }
    const PoseAt almostThere = history.at(109ms);
    ASSERT_TRUE(almostThere.found());
    EXPECT_NEAR(almostThere.position[0], 1.9F, 1e-6F);
}

// REQ-HUD-001: a frame newer than the newest pose gets nothing. Holding the last pose, or
// continuing its motion, would draw the overlay where the arm is only guessed to be.
TEST(PoseHistory, REQ_HUD_001_NeverExtrapolatesPastTheNewestSample) {
    PoseHistory history;
    fillEveryMillisecond(history, 0ms, 50ms);

    EXPECT_TRUE(history.at(50ms).found());
    const PoseAt after = history.at(50ms + 1us);
    EXPECT_FALSE(after.found());
    EXPECT_EQ(after.result, PoseLookup::AfterNewest);
    EXPECT_EQ(history.at(5s).result, PoseLookup::AfterNewest);
}

TEST(PoseHistory, REQ_HUD_001_ATimeOlderThanTheHistoryIsNotKnown) {
    PoseHistory history;
    fillEveryMillisecond(history, 100ms, 150ms);

    EXPECT_EQ(history.at(100ms).result, PoseLookup::Found);
    EXPECT_EQ(history.at(100ms - 1us).result, PoseLookup::BeforeHistory);
    EXPECT_EQ(history.at(0ms).result, PoseLookup::BeforeHistory);
}

// REQ-HUD-001: the link dropped for 200 ms and came back. Both ends of the hole are known,
// but not what the arm did in between (it braked, on its own), so nothing is said about it.
TEST(PoseHistory, REQ_HUD_001_NothingIsSaidAboutAGapLongerThanTheLimit) {
    PoseHistory history;  // maxGap 30 ms
    fillEveryMillisecond(history, 0ms, 100ms);
    fillEveryMillisecond(history, 300ms, 400ms);

    EXPECT_TRUE(history.at(99500us).found());
    EXPECT_TRUE(history.at(100ms).found());  // the last sample before the hole, exactly
    EXPECT_EQ(history.at(100ms + 1us).result, PoseLookup::InGap);
    EXPECT_EQ(history.at(200ms).result, PoseLookup::InGap);
    EXPECT_EQ(history.at(300ms - 1us).result, PoseLookup::InGap);
    EXPECT_TRUE(history.at(300ms).found());  // and the first one after it
    EXPECT_TRUE(history.at(350ms).found());
}

TEST(PoseHistory, AGapOfExactlyTheLimitIsStillOneMotion) {
    PoseHistory history({.capacity = 16, .maxGap = 30ms});
    ASSERT_TRUE(history.push(pose(0ms, 0.0F)));
    ASSERT_TRUE(history.push(pose(30ms, 3.0F)));
    ASSERT_TRUE(history.push(pose(60ms + 1ns, 6.0F)));

    const PoseAt inside = history.at(10ms);
    ASSERT_TRUE(inside.found());
    EXPECT_NEAR(inside.position[0], 1.0F, 1e-6F);
    EXPECT_EQ(history.at(45ms).result, PoseLookup::InGap);  // 30 ms + 1 ns apart
}

// A full history forgets the oldest sample, and says so when asked about that time.
TEST(PoseHistory, WhenFullTheOldestSampleMakesRoom) {
    PoseHistory history({.capacity = 8, .maxGap = 30ms});
    fillEveryMillisecond(history, 1ms, 20ms);  // 20 samples through 8 slots

    EXPECT_EQ(history.size(), 8U);
    ASSERT_TRUE(history.oldest().has_value());
    EXPECT_EQ(history.oldest()->time, 13ms);
    ASSERT_TRUE(history.newest().has_value());
    EXPECT_EQ(history.newest()->time, 20ms);

    EXPECT_EQ(history.at(12ms).result, PoseLookup::BeforeHistory);
    const PoseAt kept = history.at(16500us);
    ASSERT_TRUE(kept.found());
    EXPECT_NEAR(kept.position[0], 0.0165F, 1e-6F);
}

// The search needs the samples in time order, so a sample from the past is refused rather
// than silently breaking every later lookup.
TEST(PoseHistory, ASampleOlderThanTheNewestIsRefused) {
    PoseHistory history;
    ASSERT_TRUE(history.push(pose(10ms, 1.0F)));
    EXPECT_FALSE(history.push(pose(9ms, 9.0F)));
    EXPECT_TRUE(history.push(pose(10ms, 2.0F)));  // the same instant is not "older"
    EXPECT_EQ(history.size(), 2U);

    // Two samples at one instant: the later one is what the arm reported last.
    const PoseAt at = history.at(10ms);
    ASSERT_TRUE(at.found());
    EXPECT_FLOAT_EQ(at.position[0], 2.0F);
}

TEST(PoseHistory, ClearForgetsEverything) {
    PoseHistory history;
    fillEveryMillisecond(history, 0ms, 10ms);
    history.clear();
    EXPECT_EQ(history.size(), 0U);
    EXPECT_EQ(history.at(5ms).result, PoseLookup::NoData);
    EXPECT_TRUE(history.push(pose(1ms, 1.0F)));  // older than what was cleared: fine now
}

// Every reason reads as words for the operator's panel.
TEST(PoseHistory, EveryLookupResultHasAText) {
    for (const PoseLookup lookup : {PoseLookup::Found, PoseLookup::NoData, PoseLookup::BeforeHistory,
                                    PoseLookup::AfterNewest, PoseLookup::InGap}) {
        EXPECT_STRNE(toString(lookup), "unknown");
    }
}

}  // namespace
}  // namespace msc::hud
