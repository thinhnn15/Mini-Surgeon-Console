#include "msc/common/latency_stats.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <numeric>
#include <random>
#include <stdexcept>
#include <vector>

namespace msc {
namespace {

using std::chrono::milliseconds;

TEST(LatencyStats, REQ_VID_002_EmptyGivesNothing) {
    const LatencyStats stats(10);
    EXPECT_EQ(stats.count(), 0U);
    EXPECT_FALSE(stats.p50().has_value());
    EXPECT_FALSE(stats.p99().has_value());
    EXPECT_FALSE(stats.max().has_value());
}

TEST(LatencyStats, ZeroCapacityIsRejected) {
    EXPECT_THROW(LatencyStats(0), std::invalid_argument);
}

TEST(LatencyStats, OneSampleIsEveryPercentile) {
    LatencyStats stats(10);
    stats.add(milliseconds(7));
    EXPECT_EQ(stats.p50(), milliseconds(7));
    EXPECT_EQ(stats.p99(), milliseconds(7));
    EXPECT_EQ(stats.max(), milliseconds(7));
}

TEST(LatencyStats, REQ_VID_002_KnownPercentilesOfOneToHundred) {
    // 1..100 ms added in shuffled order: nearest rank gives exactly p50 = 50, p99 = 99.
    std::vector<int> values(100);
    std::iota(values.begin(), values.end(), 1);
    std::mt19937 rng(42);  // fixed seed: the test is the same every run
    std::shuffle(values.begin(), values.end(), rng);

    LatencyStats stats(100);
    for (const int v : values) {
        stats.add(milliseconds(v));
    }
    EXPECT_EQ(stats.count(), 100U);
    EXPECT_EQ(stats.p50(), milliseconds(50));
    EXPECT_EQ(stats.p99(), milliseconds(99));
    EXPECT_EQ(stats.max(), milliseconds(100));
    EXPECT_EQ(stats.percentile(1), milliseconds(1));
    EXPECT_EQ(stats.percentile(0), milliseconds(1));      // clamped up to 1
    EXPECT_EQ(stats.percentile(250), milliseconds(100));  // clamped down to 100
}

TEST(LatencyStats, REQ_VID_002_OneSlowFrameShowsInP99NotInP50) {
    // The reason to report p99: 99 fast frames and 1 slow one. The average (about 12 ms)
    // hides the stutter; p99 and max show it.
    LatencyStats stats(100);
    for (int i = 0; i < 99; ++i) {
        stats.add(milliseconds(10));
    }
    stats.add(milliseconds(200));
    EXPECT_EQ(stats.p50(), milliseconds(10));
    EXPECT_EQ(stats.p99(), milliseconds(10));  // 99 of 100 samples are <= 10 ms
    EXPECT_EQ(stats.max(), milliseconds(200));

    stats.add(milliseconds(200));  // a second slow frame pushes one fast one out
    EXPECT_EQ(stats.p99(), milliseconds(200));
}

TEST(LatencyStats, RollingWindowForgetsTheOldest) {
    LatencyStats stats(3);
    stats.add(milliseconds(100));
    stats.add(milliseconds(1));
    stats.add(milliseconds(2));
    EXPECT_EQ(stats.max(), milliseconds(100));

    stats.add(milliseconds(3));  // 100 drops out
    EXPECT_EQ(stats.count(), 3U);
    EXPECT_EQ(stats.max(), milliseconds(3));
    EXPECT_EQ(stats.p50(), milliseconds(2));

    stats.add(milliseconds(4));  // then 1, the next oldest
    stats.add(milliseconds(5));  // then 2
    EXPECT_EQ(stats.percentile(1), milliseconds(3));
}

TEST(LatencyStats, QueryingDoesNotChangeTheSamples) {
    LatencyStats stats(5);
    for (const int v : {5, 1, 4, 2, 3}) {
        stats.add(milliseconds(v));
    }
    EXPECT_EQ(stats.p50(), milliseconds(3));
    EXPECT_EQ(stats.max(), milliseconds(5));
    // If a query had reordered the ring buffer, the oldest (5) would not be the one evicted.
    stats.add(milliseconds(1));
    EXPECT_EQ(stats.max(), milliseconds(4));
}

TEST(LatencyStats, ClearEmptiesTheWindow) {
    LatencyStats stats(3);
    stats.add(milliseconds(1));
    stats.add(milliseconds(2));
    stats.clear();
    EXPECT_EQ(stats.count(), 0U);
    EXPECT_FALSE(stats.max().has_value());
    stats.add(milliseconds(9));
    EXPECT_EQ(stats.max(), milliseconds(9));
}

}  // namespace
}  // namespace msc
