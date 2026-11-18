#include "msc/video/synthetic_source.hpp"

#include "msc/video/scene.hpp"
#include "msc/video/test_pattern.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

using msc::ManualClock;
using msc::SteadyClock;
using msc::video::decodeSequence;
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

// ---- The delay line (M5.2): driven step by step with a fake clock ----------------------

// A frame is delivered `delay` after it was captured, and its captureTime still says when it
// was captured: that is the time the overlay must use, and the latency figures measure from.
TEST(SyntheticSource, ADelayedFrameArrivesLaterButKeepsItsCaptureTime) {
    ManualClock clock;
    SyntheticSourceConfig config = smallConfig(10ms);
    config.delay = 30ms;
    SyntheticSource source(clock, config);

    // Ticks at 0, 10 and 20 ms capture frames 1 to 3; none is 30 ms old yet.
    for (int tick = 0; tick < 3; ++tick) {
        source.produceOne();
        EXPECT_FALSE(source.acquireLatest()) << "tick " << tick;
        clock.advance(10ms);
    }
    // Tick at 30 ms: frame 1 (captured at 0) is due.
    source.produceOne();
    ASSERT_TRUE(source.acquireLatest());
    EXPECT_EQ(source.latest().sequence, 1U);
    EXPECT_EQ(source.latest().captureTime, 0ms);
    EXPECT_EQ(decodeSequence(source.latest()), std::optional<std::uint32_t>(1U));

    // From then on one frame per tick, each 30 ms old.
    for (std::uint64_t expected = 2; expected <= 6; ++expected) {
        clock.advance(10ms);
        source.produceOne();
        ASSERT_TRUE(source.acquireLatest());
        EXPECT_EQ(source.latest().sequence, expected);
        EXPECT_EQ(clock.now() - source.latest().captureTime, 30ms);
        EXPECT_EQ(decodeSequence(source.latest()),
                  std::optional<std::uint32_t>(static_cast<std::uint32_t>(expected)));
    }
    EXPECT_EQ(source.framesProduced(), 6U);
}

// A delay that is not a whole number of periods is rounded up: a frame is never delivered
// early.
TEST(SyntheticSource, ADelayBetweenTwoTicksIsRoundedUpNeverDown) {
    ManualClock clock;
    SyntheticSourceConfig config = smallConfig(10ms);
    config.delay = 25ms;
    SyntheticSource source(clock, config);

    for (int tick = 0; tick < 3; ++tick) {  // 0, 10, 20 ms
        source.produceOne();
        EXPECT_FALSE(source.acquireLatest());
        clock.advance(10ms);
    }
    source.produceOne();  // 30 ms: frame 1 is 30 ms old, the first tick at or after 25 ms
    ASSERT_TRUE(source.acquireLatest());
    EXPECT_EQ(source.latest().sequence, 1U);
}

// Delivery goes by ticks, not by the clock: after a stall, frames still come one per tick and
// in order, each `delay` periods after its own tick. Comparing times instead made a late tick
// deliver two frames at once, one of which was then never shown.
TEST(SyntheticSource, AfterAStallFramesStillComeOnePerTickInOrder) {
    ManualClock clock;
    SyntheticSourceConfig config = smallConfig(10ms);
    config.delay = 20ms;
    SyntheticSource source(clock, config);
    EXPECT_EQ(source.delayTicks(), 2U);

    source.produceOne();  // frame 1 at 0 ms
    clock.advance(10ms);
    source.produceOne();  // frame 2 at 10 ms
    EXPECT_FALSE(source.acquireLatest());
    clock.advance(100ms);  // the producer stalls
    source.produceOne();   // frame 3 at 110 ms: frame 1's turn, and only frame 1's
    ASSERT_TRUE(source.acquireLatest());
    EXPECT_EQ(source.latest().sequence, 1U);
    clock.advance(10ms);
    source.produceOne();
    ASSERT_TRUE(source.acquireLatest());
    EXPECT_EQ(source.latest().sequence, 2U);
    EXPECT_EQ(source.latest().captureTime, 10ms);
}

// Ticks without time passing (a fake clock that stands still): still one frame per tick, and
// the delay line never grows.
TEST(SyntheticSource, TheDelayLineHoldsExactlyTheDelayInFrames) {
    const ManualClock clock;
    SyntheticSourceConfig config = smallConfig(10ms);
    config.delay = 20ms;
    SyntheticSource source(clock, config);

    for (int i = 0; i < 10; ++i) {
        source.produceOne();
    }
    EXPECT_EQ(source.framesProduced(), 8U);  // 10 captured, 2 still waiting
    ASSERT_TRUE(source.acquireLatest());
    EXPECT_EQ(source.latest().sequence, 8U);
}

// The scene callback draws what else the camera sees, into the frame being captured, with its
// capture time already set.
TEST(SyntheticSource, TheSceneIsDrawnIntoEveryFrameAtItsCaptureTime) {
    ManualClock clock;
    std::vector<msc::Nanoseconds> seenAt;
    SyntheticSourceConfig config = smallConfig(10ms);
    config.delay = 10ms;
    config.scene = [&seenAt](msc::video::Frame& frame) {
        seenAt.push_back(frame.captureTime);
        // The tip moves 10 px per frame, so each frame is recognisable.
        const double x = 50.0 + (10.0 * static_cast<double>(frame.sequence));
        msc::video::drawTool(frame,
                             {.entry = {.x = 0.0, .y = 240.0},
                              .tip = {.x = x, .y = 120.0},
                              .entryRadius = 8.0,
                              .tipRadius = 4.0},
                             msc::video::barcodeRows(frame.height));
    };
    SyntheticSource source(clock, config);

    source.produceOne();  // frame 1 at 0 ms
    clock.advance(10ms);
    source.produceOne();  // frame 2 at 10 ms; frame 1 delivered
    ASSERT_TRUE(source.acquireLatest());
    EXPECT_EQ(seenAt, (std::vector<msc::Nanoseconds>{0ms, 10ms}));

    // The delivered frame shows the scene as it was when frame 1 was captured, not as it is now.
    const auto tip = msc::video::findToolTip(source.latest());
    ASSERT_TRUE(tip.has_value());
    EXPECT_NEAR(tip->x, 60.0, 0.5);
    EXPECT_EQ(decodeSequence(source.latest()), std::optional<std::uint32_t>(1U))
        << "the barcode must stay readable";
}

TEST(SyntheticSource, RejectsBadConfig) {
    const ManualClock clock;
    EXPECT_THROW(SyntheticSource(clock, SyntheticSourceConfig{.width = 8, .height = 8, .period = 10ms}),
                 std::invalid_argument);
    EXPECT_THROW(SyntheticSource(clock, smallConfig(0ms)), std::invalid_argument);
    SyntheticSourceConfig negativeDelay = smallConfig(10ms);
    negativeDelay.delay = -1ms;
    EXPECT_THROW(SyntheticSource(clock, negativeDelay), std::invalid_argument);
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
