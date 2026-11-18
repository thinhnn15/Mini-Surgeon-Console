#include "msc/comms/sequence_filter.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace msc::comms {
namespace {

constexpr std::uint32_t kSession = 0xA11CE;

TEST(SequenceFilter, REQ_COM_001_FirstPacketIsAccepted) {
    SequenceFilter filter;
    EXPECT_EQ(filter.check(kSession, 1000), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.session(), kSession);
}

TEST(SequenceFilter, InOrderPacketsAreAcceptedAndGapsCountAsLost) {
    SequenceFilter filter;
    EXPECT_EQ(filter.check(kSession, 1), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.check(kSession, 2), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.check(kSession, 5), SequenceVerdict::Accepted);  // 3 and 4 lost
    EXPECT_EQ(filter.stats().accepted, 3U);
    EXPECT_EQ(filter.stats().lost, 2U);
}

TEST(SequenceFilter, REQ_COM_001_DuplicateIsRefused) {
    SequenceFilter filter;
    ASSERT_EQ(filter.check(kSession, 10), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.check(kSession, 10), SequenceVerdict::Duplicate);
    EXPECT_EQ(filter.stats().duplicates, 1U);
}

TEST(SequenceFilter, REQ_COM_001_OlderPacketIsRefused) {
    // 12 overtook 11 on the way: 11 arrives late and must not be applied after 12.
    SequenceFilter filter;
    ASSERT_EQ(filter.check(kSession, 10), SequenceVerdict::Accepted);
    ASSERT_EQ(filter.check(kSession, 12), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.check(kSession, 11), SequenceVerdict::Old);
    EXPECT_EQ(filter.stats().old, 1U);
    EXPECT_EQ(filter.stats().lost, 1U);  // counted as lost when 12 came; it was, in effect
}

TEST(SequenceFilter, SequenceNumbersWrapAround) {
    SequenceFilter filter;
    ASSERT_EQ(filter.check(kSession, 0xFFFFFFFE), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.check(kSession, 0xFFFFFFFF), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.check(kSession, 0), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.check(kSession, 2), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.stats().lost, 1U);
    EXPECT_EQ(filter.check(kSession, 0xFFFFFFFF), SequenceVerdict::Old);

    static_assert(SequenceFilter::isNewer(0, 0xFFFFFFFF));
    static_assert(!SequenceFilter::isNewer(0xFFFFFFFF, 0));
    static_assert(!SequenceFilter::isNewer(7, 7));
}

TEST(SequenceFilter, REQ_COM_001_RestartedSenderIsFollowedAndItsOldSessionIsRetired) {
    // The sender restarts: new random session, counting from 1 again. Without sessions every
    // packet would look old for the next 4 billion packets.
    SequenceFilter filter;
    ASSERT_EQ(filter.check(kSession, 5000), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.check(0xB0B, 1), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.check(0xB0B, 2), SequenceVerdict::Accepted);
    EXPECT_EQ(filter.stats().sessionChanges, 1U);
    // A late packet from before the restart must not switch back to the old session.
    EXPECT_EQ(filter.check(kSession, 5001), SequenceVerdict::RetiredSession);
    EXPECT_EQ(filter.session(), 0xB0BU);
    EXPECT_EQ(filter.stats().retired, 1U);
}

TEST(SequenceFilter, ResetForgetsEverything) {
    SequenceFilter filter;
    ASSERT_EQ(filter.check(kSession, 5), SequenceVerdict::Accepted);
    ASSERT_EQ(filter.check(0xB0B, 1), SequenceVerdict::Accepted);
    filter.reset();
    EXPECT_FALSE(filter.session().has_value());
    EXPECT_EQ(filter.stats().accepted, 0U);
    EXPECT_EQ(filter.check(kSession, 1), SequenceVerdict::Accepted);  // no longer retired
}

}  // namespace
}  // namespace msc::comms
