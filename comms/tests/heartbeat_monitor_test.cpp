#include "msc/comms/heartbeat_monitor.hpp"

#include "msc/common/clock.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace msc::comms {
namespace {

using namespace std::chrono_literals;

TEST(HeartbeatMonitor, REQ_SAF_001_WaitingUntilTheFirstHeartbeat) {
    ManualClock clock;
    const HeartbeatMonitor monitor(10ms, 3);
    clock.advance(10s);
    EXPECT_EQ(monitor.state(clock.now()), LinkState::Waiting);
    EXPECT_FALSE(monitor.silence(clock.now()).has_value());
}

TEST(HeartbeatMonitor, REQ_SAF_001_AliveWhileHeartbeatsKeepComing) {
    ManualClock clock;
    HeartbeatMonitor monitor(10ms, 3);
    for (int i = 0; i < 100; ++i) {
        monitor.received(clock.now());
        clock.advance(10ms);
        ASSERT_EQ(monitor.state(clock.now()), LinkState::Alive) << i;
    }
}

TEST(HeartbeatMonitor, REQ_SAF_001_LostAfterThreeMissedPeriods) {
    ManualClock clock;
    HeartbeatMonitor monitor(10ms, 3);
    monitor.received(clock.now());
    clock.advance(30ms);
    EXPECT_EQ(monitor.state(clock.now()), LinkState::Alive);  // exactly 3 periods: not yet
    clock.advance(1ns);
    EXPECT_EQ(monitor.state(clock.now()), LinkState::Lost);
    EXPECT_EQ(monitor.silence(clock.now()), 30ms + 1ns);
}

TEST(HeartbeatMonitor, TwoMissedHeartbeatsAreNotALoss) {
    // One or two packets lost on the way is normal for UDP; it must not stop the arm.
    ManualClock clock;
    HeartbeatMonitor monitor(10ms, 3);
    monitor.received(clock.now());
    clock.advance(29ms);  // the next two never arrived
    monitor.received(clock.now());
    clock.advance(25ms);
    EXPECT_EQ(monitor.state(clock.now()), LinkState::Alive);
}

TEST(HeartbeatMonitor, ComesBackWhenHeartbeatsReturn) {
    ManualClock clock;
    HeartbeatMonitor monitor(10ms, 3);
    monitor.received(clock.now());
    clock.advance(1s);
    ASSERT_EQ(monitor.state(clock.now()), LinkState::Lost);
    monitor.received(clock.now());
    EXPECT_EQ(monitor.state(clock.now()), LinkState::Alive);
    monitor.reset();
    EXPECT_EQ(monitor.state(clock.now()), LinkState::Waiting);
}

}  // namespace
}  // namespace msc::comms
