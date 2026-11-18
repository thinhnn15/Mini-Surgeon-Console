#include "msc/video/video_signal.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace msc::video {
namespace {

using namespace std::chrono_literals;

TEST(VideoSignal, REQ_VID_003_NoFrameYetIsWaitingNotLive) {
    EXPECT_EQ(classifySignal(std::nullopt, Nanoseconds{5s}), VideoSignal::Waiting);
}

TEST(VideoSignal, REQ_VID_003_RecentFrameIsLive) {
    const Nanoseconds captured{10s};
    EXPECT_EQ(classifySignal(captured, captured), VideoSignal::Live);
    EXPECT_EQ(classifySignal(captured, captured + 40ms), VideoSignal::Live);
    // Exactly at the limit still counts as live; one nanosecond later it does not.
    EXPECT_EQ(classifySignal(captured, captured + kFrozenAfter), VideoSignal::Live);
    EXPECT_EQ(classifySignal(captured, captured + kFrozenAfter + 1ns), VideoSignal::Frozen);
}

TEST(VideoSignal, REQ_VID_003_OldFrameIsFrozen) {
    const Nanoseconds captured{10s};
    EXPECT_EQ(classifySignal(captured, captured + 2s), VideoSignal::Frozen);
}

TEST(VideoSignal, REQ_VID_003_SlowCameraIsNotFlaggedAsFrozen) {
    // 7.5 fps, the slowest a webcam usually gets in poor light, plus jitter.
    const Nanoseconds captured{10s};
    EXPECT_EQ(classifySignal(captured, captured + 133ms + 60ms), VideoSignal::Live);
}

TEST(VideoSignal, CustomLimit) {
    const Nanoseconds captured{1s};
    EXPECT_EQ(classifySignal(captured, captured + 80ms, 50ms), VideoSignal::Frozen);
    static_assert(classifySignal(Nanoseconds{0}, Nanoseconds{0}) == VideoSignal::Live);
}

}  // namespace
}  // namespace msc::video
