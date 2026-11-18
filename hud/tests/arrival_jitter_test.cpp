#include "msc/hud/arrival_jitter.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace msc::hud {
namespace {

using namespace std::chrono_literals;

TEST(ArrivalJitter, NothingReportedBeforeAnyPacket) {
    const ArrivalJitter jitter;
    const ArrivalJitter::Report report = jitter.report();
    EXPECT_EQ(report.samples, 0U);
    EXPECT_EQ(report.worst, 0ns);
}

// The two clocks disagree by an hour, and it does not matter: only how much later than the
// quickest packet each one arrived is counted.
TEST(ArrivalJitter, MeasuresLatenessAgainstTheQuickestPacketWhateverTheClocksSay) {
    ArrivalJitter jitter;
    const Nanoseconds clockOffset = 1h;
    const Nanoseconds quickestTrip = 200us;
    Nanoseconds armTime = 5s;
    const auto send = [&](Nanoseconds extraDelay) {
        jitter.add(armTime + clockOffset + quickestTrip + extraDelay, armTime);
        armTime += 1ms;
    };

    send(0us);     // the quickest
    send(300us);   // a little later: under every threshold
    send(1500us);  // over 1 ms
    send(2500us);  // over 1 and 2 ms
    send(7ms);     // over all three
    send(0us);

    const ArrivalJitter::Report report = jitter.report();
    EXPECT_EQ(report.samples, 6U);
    EXPECT_EQ(report.over1ms, 3U);
    EXPECT_EQ(report.over2ms, 2U);
    EXPECT_EQ(report.over5ms, 1U);
    EXPECT_EQ(report.worst, 7ms);
}

// A quicker packet later on lowers the baseline for the packets after it. Earlier packets were
// judged against the quickest known then: the figures can only understate lateness at the start.
TEST(ArrivalJitter, AQuickerPacketLaterBecomesTheNewBaseline) {
    ArrivalJitter jitter;
    jitter.add(10ms + 3ms, 10ms);  // 3 ms trip: the quickest so far, so "on time"
    jitter.add(11ms + 1ms, 11ms);  // 1 ms trip: the new quickest
    jitter.add(12ms + 3ms, 12ms);  // 3 ms trip again: now 2 ms late
    const ArrivalJitter::Report report = jitter.report();
    EXPECT_EQ(report.over1ms, 1U);
    EXPECT_EQ(report.worst, 2ms);
}

}  // namespace
}  // namespace msc::hud
