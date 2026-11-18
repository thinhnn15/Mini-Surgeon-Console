#include "msc/hud/pose_feed.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace msc::hud {
namespace {

using namespace std::chrono_literals;

TimedPose pose(Nanoseconds time, float value) {
    TimedPose p;
    p.time = time;
    p.position.fill(value);
    return p;
}

TEST(PoseFeed, BeforeTheFirstPoseNeitherReaderGetsAnything) {
    PoseFeed feed;
    TimedPose out = pose(7ms, 7.0F);
    EXPECT_FALSE(feed.next(out));
    EXPECT_FALSE(feed.newest(out));
    EXPECT_EQ(out.time, 7ms) << "a failed read must leave `out` alone";
}

// The two readers want different things from the same stream: the history every pose in
// order, the camera only the newest.
TEST(PoseFeed, TheHistoryGetsEveryPoseAndTheCameraTheNewest) {
    PoseFeed feed;
    feed.publish(pose(1ms, 0.1F));
    feed.publish(pose(2ms, 0.2F));
    feed.publish(pose(3ms, 0.3F));

    TimedPose newest;
    ASSERT_TRUE(feed.newest(newest));
    EXPECT_EQ(newest.time, 3ms);
    EXPECT_FLOAT_EQ(newest.position[0], 0.3F);
    // Asking again without a new pose: still the newest, not "nothing".
    ASSERT_TRUE(feed.newest(newest));
    EXPECT_EQ(newest.time, 3ms);

    for (const Nanoseconds expected : {Nanoseconds{1ms}, Nanoseconds{2ms}, Nanoseconds{3ms}}) {
        TimedPose next;
        ASSERT_TRUE(feed.next(next));
        EXPECT_EQ(next.time, expected);
    }
    TimedPose none;
    EXPECT_FALSE(feed.next(none));
    EXPECT_EQ(feed.refused(), 0U);
}

// Nobody is drawing (a minimised window): the queue fills, new poses are refused and counted,
// and the link thread carries on. The camera's mailbox is not affected.
TEST(PoseFeed, AFullQueueRefusesAndCountsButTheNewestStillGetsThrough) {
    PoseFeed feed(4);
    for (int i = 1; i <= 10; ++i) {
        feed.publish(pose(Nanoseconds{std::chrono::milliseconds(i)}, static_cast<float>(i)));
    }
    EXPECT_EQ(feed.refused(), 6U);

    TimedPose newest;
    ASSERT_TRUE(feed.newest(newest));
    EXPECT_EQ(newest.time, 10ms);

    // The queue kept the oldest four: the reader finds a hole after them, which the history
    // treats as "not known" (PoseHistory.REQ_HUD_001_NothingIsSaidAboutAGapLongerThanTheLimit).
    TimedPose next;
    ASSERT_TRUE(feed.next(next));
    EXPECT_EQ(next.time, 1ms);
}

}  // namespace
}  // namespace msc::hud
