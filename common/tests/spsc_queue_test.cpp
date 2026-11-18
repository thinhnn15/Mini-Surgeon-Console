#include "msc/common/spsc_queue.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <thread>

namespace {

// ThreadSanitizer makes every memory access several times slower; keep its run short.
#if defined(__SANITIZE_THREAD__)
constexpr std::uint64_t kStressIterations = 100'000;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr std::uint64_t kStressIterations = 100'000;
#else
constexpr std::uint64_t kStressIterations = 1'000'000;
#endif
#else
constexpr std::uint64_t kStressIterations = 1'000'000;
#endif

// Bigger than any single CPU store, so a torn read would show up as fields that disagree.
struct Sample {
    std::uint64_t a;
    std::uint64_t b;
    std::uint64_t c;
    std::uint64_t d;
};

}  // namespace

TEST(SpscQueue, PopFromAnEmptyQueueGivesNothingAndLeavesOutAlone) {
    msc::SpscQueue<int> queue(4);
    int out = -1;
    EXPECT_FALSE(queue.tryPop(out));
    EXPECT_EQ(out, -1);
}

TEST(SpscQueue, CapacityIsRoundedUpToAPowerOfTwo) {
    EXPECT_EQ(msc::SpscQueue<int>(0).capacity(), 2U);
    EXPECT_EQ(msc::SpscQueue<int>(4).capacity(), 4U);
    EXPECT_EQ(msc::SpscQueue<int>(5).capacity(), 8U);
    EXPECT_EQ(msc::SpscQueue<int>(1000).capacity(), 1024U);
}

// The opposite of LatestValue: nothing is dropped, and the order is kept.
TEST(SpscQueue, EveryValueComesOutOnceInTheOrderItWentIn) {
    msc::SpscQueue<int> queue(8);
    for (int i = 1; i <= 5; ++i) {
        ASSERT_TRUE(queue.tryPush(i));
    }
    for (int i = 1; i <= 5; ++i) {
        int out = 0;
        ASSERT_TRUE(queue.tryPop(out));
        EXPECT_EQ(out, i);
    }
    int out = 0;
    EXPECT_FALSE(queue.tryPop(out));
}

// A full queue refuses the new value: the writer is told, never blocked, and what is
// already queued is not disturbed.
TEST(SpscQueue, AFullQueueRefusesTheNewValueAndKeepsTheOldOnes) {
    msc::SpscQueue<int> queue(4);
    for (int i = 1; i <= 4; ++i) {
        ASSERT_TRUE(queue.tryPush(i));
    }
    EXPECT_FALSE(queue.tryPush(5));

    int out = 0;
    ASSERT_TRUE(queue.tryPop(out));
    EXPECT_EQ(out, 1);
    // One slot is free again.
    EXPECT_TRUE(queue.tryPush(6));
    EXPECT_FALSE(queue.tryPush(7));
    for (const int expected : {2, 3, 4, 6}) {
        ASSERT_TRUE(queue.tryPop(out));
        EXPECT_EQ(out, expected);
    }
}

// The counters keep counting up while the slots are reused: go round the ring many times.
TEST(SpscQueue, KeepsWorkingAfterGoingRoundTheRingManyTimes) {
    msc::SpscQueue<int> queue(4);
    int next = 0;
    int expected = 0;
    for (int round = 0; round < 1000; ++round) {
        for (int i = 0; i < 3; ++i) {
            ASSERT_TRUE(queue.tryPush(next++));
        }
        for (int i = 0; i < 3; ++i) {
            int out = -1;
            ASSERT_TRUE(queue.tryPop(out));
            ASSERT_EQ(out, expected++);
        }
    }
}

// Under real concurrency: the writer pushes 1, 2, 3, ... (trying again when the queue is
// full), the reader pops as fast as it can. Every value must arrive once, whole and in order.
// Run under ThreadSanitizer (linux-tsan preset) this also proves there is no data race.
TEST(SpscQueue, TwoThreadsLoseNothingTearNothingAndKeepTheOrder) {
    msc::SpscQueue<Sample> queue(64);
    std::uint64_t refused = 0;

    {
        const std::jthread writer([&queue, &refused] {
            for (std::uint64_t i = 1; i <= kStressIterations; ++i) {
                while (!queue.tryPush(Sample{i, i, i, i})) {
                    ++refused;  // full: the reader has not caught up yet
                    std::this_thread::yield();
                }
            }
        });

        std::uint64_t torn = 0;
        std::uint64_t outOfOrder = 0;
        std::uint64_t last = 0;
        while (last != kStressIterations) {
            Sample s{};
            if (!queue.tryPop(s)) {
                continue;
            }
            if (s.a != s.b || s.a != s.c || s.a != s.d) {
                ++torn;
            }
            if (s.a != last + 1) {
                ++outOfOrder;  // a lost, repeated or reordered value
            }
            last = s.a;
        }
        EXPECT_EQ(torn, 0U);
        EXPECT_EQ(outOfOrder, 0U);
    }  // the writer has finished here: `refused` is safe to read

    std::cout << "[          ] pushes refused by a full queue (and tried again): " << refused << " of "
              << kStressIterations << '\n';
}
