#include "msc/common/clock.hpp"

#include <gtest/gtest.h>

using namespace std::chrono_literals;

TEST(SteadyClock, NeverGoesBackwards) {
    const msc::SteadyClock clock;
    auto previous = clock.now();
    for (int i = 0; i < 10'000; ++i) {
        const auto current = clock.now();
        ASSERT_GE(current, previous);
        previous = current;
    }
}

TEST(ManualClock, OnlyMovesWhenAdvanced) {
    msc::ManualClock clock;
    EXPECT_EQ(clock.now(), 0ns);
    clock.advance(10ms);
    EXPECT_EQ(clock.now(), 10ms);
    EXPECT_EQ(clock.now(), 10ms);
}
