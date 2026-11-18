#include "msc/common/triple_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

#if defined(__SANITIZE_THREAD__)
constexpr std::uint32_t kStressIterations = 20'000;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
constexpr std::uint32_t kStressIterations = 20'000;
#else
constexpr std::uint32_t kStressIterations = 200'000;
#endif
#else
constexpr std::uint32_t kStressIterations = 200'000;
#endif

constexpr std::size_t kPayloadSize = 256;

}  // namespace

TEST(TripleBuffer, StartsWithInitialValueInEverySlot) {
    msc::TripleBuffer<std::vector<int>> buffer(std::vector<int>(4, 7));
    EXPECT_EQ(buffer.readSlot(), std::vector<int>(4, 7));
    EXPECT_EQ(buffer.writeSlot(), std::vector<int>(4, 7));
    EXPECT_FALSE(buffer.acquireLatest());
}

TEST(TripleBuffer, WriterFillsInPlaceAndReaderSeesIt) {
    msc::TripleBuffer<std::vector<int>> buffer(std::vector<int>(3, 0));
    std::ranges::fill(buffer.writeSlot(), 5);
    buffer.publish();

    ASSERT_TRUE(buffer.acquireLatest());
    EXPECT_EQ(buffer.readSlot(), std::vector<int>(3, 5));
    EXPECT_FALSE(buffer.acquireLatest());
}

// The property zero-copy rendering relies on: what the reader holds does not change under
// it, no matter how often the writer publishes, until the reader asks for a new value.
TEST(TripleBuffer, ReadSlotIsStableUntilNextAcquire) {
    msc::TripleBuffer<int> buffer;
    buffer.writeSlot() = 1;
    buffer.publish();
    ASSERT_TRUE(buffer.acquireLatest());
    const int* held = &buffer.readSlot();

    for (int i = 2; i <= 100; ++i) {
        buffer.writeSlot() = i;
        buffer.publish();
    }
    EXPECT_EQ(&buffer.readSlot(), held);
    EXPECT_EQ(buffer.readSlot(), 1);

    ASSERT_TRUE(buffer.acquireLatest());
    EXPECT_EQ(buffer.readSlot(), 100);
}

// No allocation after construction: the three buffers are reused, never replaced.
TEST(TripleBuffer, ReusesTheSameThreeBuffers) {
    msc::TripleBuffer<std::vector<int>> buffer(std::vector<int>(16, 0));
    std::vector<const int*> seen;
    for (int i = 0; i < 30; ++i) {
        buffer.writeSlot()[0] = i;
        buffer.publish();
        ASSERT_TRUE(buffer.acquireLatest());
        const int* data = buffer.readSlot().data();
        if (std::ranges::find(seen, data) == seen.end()) {
            seen.push_back(data);
        }
    }
    EXPECT_LE(seen.size(), 3U);
}

// In-place use with a type that is not trivially copyable (a heap buffer, like a frame).
// The writer fills every element of its slot with i; the reader checks that every element
// of the slot it holds agrees and that i only moves forward. Under ThreadSanitizer this
// also shows the slot hand-over is properly synchronised.
TEST(TripleBuffer, InPlaceUseNeverTearsUnderConcurrency) {
    msc::TripleBuffer<std::vector<std::uint32_t>> buffer(std::vector<std::uint32_t>(kPayloadSize, 0));

    const std::jthread writer([&buffer] {
        for (std::uint32_t i = 1; i <= kStressIterations; ++i) {
            std::ranges::fill(buffer.writeSlot(), i);
            buffer.publish();
        }
    });

    std::uint32_t torn = 0;
    std::uint32_t backwards = 0;
    std::uint32_t last = 0;
    while (last != kStressIterations) {
        if (!buffer.acquireLatest()) {
            continue;
        }
        const auto& slot = buffer.readSlot();
        const std::uint32_t value = slot.front();
        if (std::ranges::any_of(slot, [value](std::uint32_t v) { return v != value; })) {
            ++torn;
        }
        if (value <= last) {
            ++backwards;
        }
        last = value;
    }

    EXPECT_EQ(torn, 0U);
    EXPECT_EQ(backwards, 0U);
}
