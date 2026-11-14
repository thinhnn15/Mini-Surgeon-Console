#include "msc/common/latest_value.hpp"

#include <gtest/gtest.h>

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

TEST(LatestValue, ReadBeforeAnyPublishGivesValueInitialisedAndNotFresh) {
    msc::LatestValue<int> box;
    int out = -1;
    EXPECT_FALSE(box.read(out));
    EXPECT_EQ(out, 0);
}

// REQ-VID-001: the newest value wins; older ones are dropped, not queued.
TEST(LatestValue, REQ_VID_001_ReaderSeesNewestValue) {
    msc::LatestValue<int> box;
    box.publish(1);
    box.publish(2);
    box.publish(3);

    int out = 0;
    EXPECT_TRUE(box.read(out));
    EXPECT_EQ(out, 3);

    // Nothing new: same value again, and read says so.
    EXPECT_FALSE(box.read(out));
    EXPECT_EQ(out, 3);
}

TEST(LatestValue, EachPublishIsSeenOnceWhenReaderKeepsUp) {
    msc::LatestValue<int> box;
    for (int i = 0; i < 10; ++i) {
        box.publish(i);
        int out = -1;
        ASSERT_TRUE(box.read(out));
        EXPECT_EQ(out, i);
        EXPECT_FALSE(box.read(out));
    }
}

// REQ-VID-001 under real concurrency. The writer publishes {i, i, i, i} as fast as it can;
// the reader spins on read(). If a read ever mixed two publishes, the fields would differ.
// Run under ThreadSanitizer (linux-tsan preset) this also proves there is no data race.
TEST(LatestValue, REQ_VID_001_NeverTearsAndNeverGoesBackwards) {
    msc::LatestValue<Sample> box;

    std::jthread writer([&box] {
        for (std::uint64_t i = 1; i <= kStressIterations; ++i) {
            box.publish(Sample{i, i, i, i});
        }
    });

    std::uint64_t torn = 0;
    std::uint64_t backwards = 0;
    std::uint64_t freshReads = 0;
    std::uint64_t last = 0;
    // The writer's last publish always leaves the fresh bit set, so the reader is
    // guaranteed to see kStressIterations eventually: this loop terminates.
    while (last != kStressIterations) {
        Sample s{};
        if (!box.read(s)) {
            continue;
        }
        ++freshReads;
        if (s.a != s.b || s.a != s.c || s.a != s.d) {
            ++torn;
        }
        if (s.a <= last) {
            ++backwards;  // a fresh read must be strictly newer than the previous one
        }
        last = s.a;
    }

    EXPECT_EQ(torn, 0U);
    EXPECT_EQ(backwards, 0U);
    // Most values are dropped (the reader cannot keep up with a writer in a tight loop);
    // that is the intended behaviour. Print how many got through.
    std::cout << "[          ] fresh reads: " << freshReads << " of " << kStressIterations << '\n';
}
