#include "msc/soak/trend.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace {

using msc::soak::Point;

constexpr double kMiB = 1024.0 * 1024.0;

// One sample every `every` seconds for `duration` seconds.
std::vector<Point> samples(double duration, double every, const std::function<double(double)>& value) {
    std::vector<Point> points;
    for (double t = 0.0; t <= duration; t += every) {
        points.push_back({t, value(t)});
    }
    return points;
}

// The limits soak_check uses for resident memory.
const msc::soak::GrowthLimit kMemory{.warmUp = 30.0, .maxPerHour = 32 * kMiB, .noise = 4 * kMiB};

}  // namespace

TEST(Trend, FitsAStraightLine) {
    const auto points = samples(10.0, 1.0, [](double t) { return (3.0 * t) + 2.0; });
    const auto line = msc::soak::fitLine(points);
    ASSERT_TRUE(line);
    EXPECT_NEAR(line->slope, 3.0, 1e-9);
    EXPECT_NEAR(line->intercept, 2.0, 1e-9);
    EXPECT_FALSE(msc::soak::fitLine(std::vector<Point>{{1.0, 5.0}}));
    EXPECT_FALSE(msc::soak::fitLine(std::vector<Point>{{1.0, 5.0}, {1.0, 6.0}}));
}

TEST(Trend, REQ_SYS_001_ALeakIsStillGrowingAtTheEnd) {
    // 100 kB a second on top of 200 MB: what a leak of one frame per few seconds looks like.
    const auto points = samples(120.0, 5.0, [](double t) { return (200 * kMiB) + (t * 100e3); });
    const auto verdict = msc::soak::judgeGrowth(points, kMemory);
    EXPECT_FALSE(verdict.ok);
    EXPECT_NEAR(verdict.perHour, 360e6, 1e3);
    EXPECT_NE(verdict.why.find("still growing at the end"), std::string::npos) << verdict.why;
}

TEST(Trend, REQ_SYS_001_ACacheThatFillsUpAndThenStaysFlatIsNotALeak) {
    // Grows by 50 MB over the first minute (textures, pools, a cache), then nothing.
    const auto points =
        samples(180.0, 5.0, [](double t) { return (200 * kMiB) + (std::min(t, 60.0) / 60.0 * 50 * kMiB); });
    const auto verdict = msc::soak::judgeGrowth(points, kMemory);
    EXPECT_TRUE(verdict.ok) << verdict.why;
    EXPECT_NEAR(verdict.perHour, 0.0, 1.0);
    // The same samples judged from the start (no warm-up, all of them) would call it a leak.
    const auto naive =
        msc::soak::judgeGrowth(std::vector<Point>(points.begin(), points.begin() + 12),
                               {.warmUp = 0.0, .maxPerHour = kMemory.maxPerHour, .noise = kMemory.noise});
    EXPECT_FALSE(naive.ok);
}

TEST(Trend, REQ_SYS_001_NoiseIsNotATrend) {
    // +-1.5 MB up and down around a flat line, and one step of 2 MB in the middle.
    const auto points = samples(120.0, 5.0, [](double t) {
        const double wiggle = (static_cast<int>(t / 5.0) % 2 == 0 ? 1.5 : -1.5) * kMiB;
        return (200 * kMiB) + wiggle + (t > 90.0 ? 2 * kMiB : 0.0);
    });
    EXPECT_TRUE(msc::soak::judgeGrowth(points, kMemory).ok);
}

TEST(Trend, SlowGrowthUnderTheRateLimitIsAccepted) {
    // 16 MB per hour over an hour: under the 32 MB/h limit, although it rises more than the noise.
    const auto points =
        samples(3600.0, 10.0, [](double t) { return (200 * kMiB) + (t / 3600.0 * 16 * kMiB); });
    const auto verdict = msc::soak::judgeGrowth(points, kMemory);
    EXPECT_TRUE(verdict.ok) << verdict.why;
    EXPECT_NEAR(verdict.perHour, 16 * kMiB, 1e3);
}

TEST(Trend, TooFewSamplesAreNotJudgedAndSaySo) {
    const auto points = samples(40.0, 5.0, [](double t) { return t * kMiB; });
    const auto verdict = msc::soak::judgeGrowth(points, kMemory);
    EXPECT_TRUE(verdict.ok);
    EXPECT_NE(verdict.why.find("too short to judge"), std::string::npos) << verdict.why;
}

TEST(Trend, REQ_SYS_001_ALatencyThatGetsWorseIsReported) {
    // p99 from 8 ms to 20 ms over two minutes: a queue building up somewhere.
    const auto points = samples(150.0, 5.0, [](double t) { return 8.0 + (t / 10.0); });
    const auto verdict = msc::soak::judgeDrift(points, {.warmUp = 30.0, .maxIncrease = 3.0, .maxRatio = 1.5});
    EXPECT_FALSE(verdict.ok);
    EXPECT_NE(verdict.why.find("got worse"), std::string::npos) << verdict.why;
}

TEST(Trend, REQ_SYS_001_ASteadyOrSlightlyWorseLatencyIsNotDrift) {
    const msc::soak::DriftLimit limit{.warmUp = 30.0, .maxIncrease = 3.0, .maxRatio = 1.5};
    // Flat with one spike: medians ignore it.
    const auto spike = samples(150.0, 5.0, [](double t) { return t == 100.0 ? 40.0 : 9.0; });
    EXPECT_TRUE(msc::soak::judgeDrift(spike, limit).ok);
    // From 1.0 to 1.8 ms: x1.8, but only +0.8 ms.
    const auto small = samples(150.0, 5.0, [](double t) { return 1.0 + (t / 150.0 * 0.8); });
    EXPECT_TRUE(msc::soak::judgeDrift(small, limit).ok);
    // From 30 to 34 ms: +4 ms, but only x1.13.
    const auto large = samples(150.0, 5.0, [](double t) { return 30.0 + (t / 150.0 * 4.0); });
    EXPECT_TRUE(msc::soak::judgeDrift(large, limit).ok);
}
