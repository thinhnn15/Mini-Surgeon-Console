#include "msc/video/synthetic_source.hpp"

#include "msc/video/test_pattern.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

using msc::ManualClock;
using msc::SteadyClock;
using msc::video::SyntheticSource;
using msc::video::SyntheticSourceConfig;
using namespace std::chrono_literals;

namespace {

// Small frames keep the tests fast, also under ThreadSanitizer.
SyntheticSourceConfig smallConfig(std::chrono::nanoseconds period) {
    return SyntheticSourceConfig{.width = 320, .height = 240, .period = period};
}

// Polls until `done()` or the timeout. Generous timeout: CI machines can be slow, and a
// test that fails only on a busy machine is worse than one that takes a second longer.
template <typename Pred>
bool waitFor(Pred done, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!done()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(1ms);
    }
    return true;
}

}  // namespace

// ---- Driven step by step, no thread: fully deterministic ------------------------------

TEST(SyntheticSource, NothingBeforeTheFirstFrame) {
    const ManualClock clock;
    SyntheticSource source(clock, smallConfig(10ms));
    EXPECT_FALSE(source.acquireLatest());
    EXPECT_EQ(source.latest().sequence, 0U);
}

TEST(SyntheticSource, ProduceOneStampsTimeAndSequence) {
    ManualClock clock;
    SyntheticSource source(clock, smallConfig(10ms));

    clock.advance(5ms);
    source.produceOne();
    ASSERT_TRUE(source.acquireLatest());
    EXPECT_EQ(source.latest().sequence, 1U);
    EXPECT_EQ(source.latest().captureTime, 5ms);
    EXPECT_EQ(source.latest().width, 320U);
    EXPECT_EQ(msc::video::decodeSequence(source.latest()), 1U);

    clock.advance(10ms);
    source.produceOne();
    ASSERT_TRUE(source.acquireLatest());
    EXPECT_EQ(source.latest().sequence, 2U);
    EXPECT_EQ(source.latest().captureTime, 15ms);
}

TEST(SyntheticSource, ReaderGetsNewestAndOlderFramesAreDropped) {
    const ManualClock clock;
    SyntheticSource source(clock, smallConfig(10ms));
    for (int i = 0; i < 5; ++i) {
        source.produceOne();
    }
    ASSERT_TRUE(source.acquireLatest());
    EXPECT_EQ(source.latest().sequence, 5U);
    EXPECT_FALSE(source.acquireLatest());
}

TEST(SyntheticSource, RejectsBadConfig) {
    const ManualClock clock;
    EXPECT_THROW(SyntheticSource(clock, SyntheticSourceConfig{.width = 8, .height = 8, .period = 10ms}),
                 std::invalid_argument);
    EXPECT_THROW(SyntheticSource(clock, smallConfig(0ms)), std::invalid_argument);
}

// ---- With the real producer thread ----------------------------------------------------

TEST(SyntheticSource, ThreadProducesIncreasingFrames) {
    const SteadyClock clock;
    SyntheticSource source(clock, smallConfig(2ms));
    source.start();

    std::uint64_t last = 0;
    int newFrames = 0;
    ASSERT_TRUE(waitFor([&] {
        if (source.acquireLatest()) {
            const auto& frame = source.latest();
            EXPECT_GT(frame.sequence, last);
            EXPECT_EQ(msc::video::decodeSequence(frame), static_cast<std::uint32_t>(frame.sequence));
            last = frame.sequence;
            ++newFrames;
        }
        return newFrames >= 10;
    }));
    source.stop();
}

TEST(SyntheticSource, RunsAtRoughlyTheConfiguredRate) {
    const SteadyClock clock;
    SyntheticSource source(clock, smallConfig(5ms));
    source.start();
    std::this_thread::sleep_for(300ms);
    source.stop();
    std::cout << "[          ] frames in 300 ms at a 5 ms period: " << source.framesProduced() << '\n';

    // 300 ms / 5 ms = 60 frames. Wide bounds: this checks "paced, not flat out and not
    // stalled", not timing precision, which a shared CI machine cannot promise.
    EXPECT_GE(source.framesProduced(), 20U);
    EXPECT_LE(source.framesProduced(), 80U);
}

// stop() must not wait out the rest of a long period.
TEST(SyntheticSource, StopReturnsPromptlyEvenWithALongPeriod) {
    const SteadyClock clock;
    SyntheticSource source(clock, smallConfig(10s));
    source.start();
    ASSERT_TRUE(waitFor([&] { return source.framesProduced() >= 1; }));

    const auto before = std::chrono::steady_clock::now();
    source.stop();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 1s);
}

TEST(SyntheticSource, StartAndStopAreSafeToRepeat) {
    const SteadyClock clock;
    SyntheticSource source(clock, smallConfig(2ms));
    source.stop();  // not started: no-op
    source.start();
    source.start();  // already running: no-op
    ASSERT_TRUE(waitFor([&] { return source.framesProduced() >= 3; }));
    source.stop();
    source.stop();
    const auto produced = source.framesProduced();
    source.start();  // restart continues the sequence
    ASSERT_TRUE(waitFor([&] { return source.framesProduced() > produced; }));
}  // destructor stops the running thread

TEST(SyntheticSource, DestructorStopsTheThread) {
    const SteadyClock clock;
    {
        SyntheticSource source(clock, smallConfig(1ms));
        source.start();
        ASSERT_TRUE(waitFor([&] { return source.framesProduced() >= 3; }));
    }
    SUCCEED();  // reaching here without a hang or a sanitizer report is the test
}
