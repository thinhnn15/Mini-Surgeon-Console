#include "msc/teleop/teleop_core.hpp"

#include "msc/common/clock.hpp"
#include "msc/comms/arm_link.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>

namespace msc::teleop {
namespace {

using namespace std::chrono_literals;
using comms::ArmState;

JointArray all(float value) {
    JointArray a{};
    a.fill(value);
    return a;
}

JointArray first(float value) {
    JointArray a{};
    a[0] = value;
    return a;
}

// Stands in for ArmLink: records what the core commands, and plays the arm's side of the status.
class FakeArm final : public comms::ArmPort {
public:
    void move(const JointArray& target) noexcept override {
        moving = true;
        target_ = target;
    }
    void hold() noexcept override { moving = false; }
    bool readStatus(comms::ArmLinkStatus& status) noexcept override {
        if (!fresh_) {
            return false;
        }
        status = status_;
        fresh_ = false;
        return true;
    }

    // The arm answers: link alive, in `state`, at `position`, acknowledging a new command.
    void answer(ArmState state, const JointArray& position) {
        status_.link = comms::LinkState::Alive;
        status_.telemetrySeen = true;
        status_.telemetry.state = state;
        status_.telemetry.position = position;
        ++status_.telemetry.lastCommandSequence;
        fresh_ = true;
    }
    // The arm answers without acknowledging anything new (it stopped applying our commands).
    void answerWithoutAck(ArmState state) {
        status_.link = comms::LinkState::Alive;
        status_.telemetry.state = state;
        fresh_ = true;
    }
    void linkLost() {
        status_.link = comms::LinkState::Lost;
        fresh_ = true;
    }

    bool moving = false;
    [[nodiscard]] const JointArray& target() const { return target_; }

private:
    comms::ArmLinkStatus status_;
    JointArray target_{};
    bool fresh_ = false;
};

// A core with a live arm at `armAt` and a hand reporting from 0, ticked by hand.
class Fixture : public ::testing::Test {
protected:
    void step(Nanoseconds dt = 5ms, bool armAcks = true, ArmState armState = ArmState::Following) {
        clock.advance(dt);
        if (armAcks) {
            arm.answer(armState, armAt);
        }
        core.submitHand(HandSample{.pose = hand, .clutch = clutch, .time = clock.now()});
        core.tick();
    }
    TeleopStatus status() {
        TeleopStatus s;
        static_cast<void>(core.readStatus(s));
        return s;
    }
    void engage() {
        step();
        core.request(Request::Engage);
        step();
        ASSERT_EQ(status().state, StateKind::Engaged) << status().lastRefusal;
    }

    ManualClock clock;
    FakeArm arm;
    TeleopCore core{clock, arm};
    JointArray hand{};
    JointArray armAt = all(0.5F);
    bool clutch = false;
};

TEST_F(Fixture, IdleTellsTheArmToHold) {
    step();
    EXPECT_FALSE(arm.moving);
    EXPECT_EQ(status().state, StateKind::Idle);
    EXPECT_TRUE(status().engageAllowed);
}

TEST_F(Fixture, EngageIsRefusedWithoutALiveArm) {
    clock.advance(5ms);
    core.submitHand(HandSample{.time = clock.now()});
    core.tick();  // no answer from the arm at all
    core.request(Request::Engage);
    clock.advance(5ms);
    core.submitHand(HandSample{.time = clock.now()});
    core.tick();
    EXPECT_EQ(status().state, StateKind::Idle);
    EXPECT_FALSE(status().lastRefusal.empty());
    EXPECT_FALSE(arm.moving);
}

TEST_F(Fixture, EngagedHandMotionMovesTheArmScaled) {
    engage();
    EXPECT_TRUE(arm.moving);
    EXPECT_EQ(arm.target(), all(0.5F));  // taking over does not move the arm
    hand = first(0.4F);
    step();
    EXPECT_NEAR(arm.target()[0], 0.7F, 1e-6F);
    EXPECT_EQ(arm.target()[1], 0.5F);
}

TEST_F(Fixture, REQ_SAF_003_WhilePausedTheArmHoldsWhateverTheHandDoes) {
    engage();
    clutch = true;
    step();
    EXPECT_EQ(status().state, StateKind::Paused);
    for (const float h : {0.3F, -0.6F, 1.0F}) {
        hand = all(h);
        step();
        EXPECT_FALSE(arm.moving) << h;
        EXPECT_FALSE(status().commandingMove);
    }
    // Release with the hand somewhere else: re-indexed to where the arm is, no jump.
    armAt = all(0.55F);
    clutch = false;
    step();
    EXPECT_EQ(status().state, StateKind::Engaged);
    EXPECT_TRUE(arm.moving);
    EXPECT_EQ(arm.target(), all(0.55F));
}

TEST_F(Fixture, REQ_SAF_002_ArmLinkLostFaultsAndHolds) {
    engage();
    clock.advance(5ms);
    arm.linkLost();
    core.submitHand(HandSample{.pose = hand, .time = clock.now()});
    core.tick();
    const TeleopStatus s = status();
    EXPECT_EQ(s.state, StateKind::Fault);
    EXPECT_EQ(s.fault, FaultCode::ArmLinkLost);
    EXPECT_FALSE(arm.moving);
}

TEST_F(Fixture, REQ_SAF_002_StaleHandInputFaultsAfterItsTimeout) {
    engage();
    // The hand stops reporting; the arm keeps answering.
    for (Nanoseconds t{0}; t < 95ms; t += 5ms) {
        clock.advance(5ms);
        arm.answer(ArmState::Following, armAt);
        core.tick();
    }
    EXPECT_EQ(status().state, StateKind::Engaged);  // 100 ms not reached yet
    for (int i = 0; i < 2; ++i) {
        clock.advance(5ms);
        arm.answer(ArmState::Following, armAt);
        core.tick();
    }
    EXPECT_EQ(status().state, StateKind::Fault);
    EXPECT_EQ(status().fault, FaultCode::HandInputStale);
    EXPECT_FALSE(arm.moving);
}

TEST_F(Fixture, REQ_SAF_002_CommandsNotAcknowledgedFault) {
    engage();
    for (Nanoseconds t{0}; t < 95ms; t += 5ms) {
        step(5ms, false);  // telemetry stops confirming
        arm.answerWithoutAck(ArmState::Following);
    }
    EXPECT_EQ(status().state, StateKind::Engaged);
    step(10ms, false);
    EXPECT_EQ(status().state, StateKind::Fault);
    EXPECT_EQ(status().fault, FaultCode::CommandNotAcknowledged);
}

TEST_F(Fixture, REQ_SAF_002_ArmHoldingOnItsOwnWhileEngagedFaults) {
    engage();
    // Just after engaging the arm still reports Holding (from the console's hold commands):
    // allowed for holdingTimeout, it has to receive the first move.
    for (Nanoseconds t{0}; t < 90ms; t += 5ms) {
        step(5ms, true, ArmState::Holding);
    }
    EXPECT_EQ(status().state, StateKind::Engaged);
    step(15ms, true, ArmState::Holding);
    EXPECT_EQ(status().state, StateKind::Fault);
    EXPECT_EQ(status().fault, FaultCode::ArmHoldingOnItsOwn);
}

TEST_F(Fixture, REQ_SAF_002_VideoFrozenWhileEngagedFaults) {
    engage();
    core.setVideoLive(false);
    step();
    EXPECT_EQ(status().state, StateKind::Fault);
    EXPECT_EQ(status().fault, FaultCode::VideoFrozen);
}

TEST_F(Fixture, InIdleAFaultConditionOnlyBlocksEngaging) {
    step();
    core.setVideoLive(false);
    step();
    EXPECT_EQ(status().state, StateKind::Idle);
    EXPECT_FALSE(status().engageAllowed);
    core.request(Request::Engage);
    step();
    EXPECT_EQ(status().state, StateKind::Idle);
    EXPECT_FALSE(status().lastRefusal.empty());
}

TEST_F(Fixture, REQ_SAF_002_ResetIsRefusedWhileTheCauseIsPresentThenAccepted) {
    engage();
    core.setInjected(faultSet(FaultCode::Injected));
    step();
    ASSERT_EQ(status().state, StateKind::Fault);
    EXPECT_FALSE(status().resetAllowed);
    core.request(Request::Reset);
    step();
    EXPECT_EQ(status().state, StateKind::Fault);
    EXPECT_FALSE(status().lastRefusal.empty());

    // The cause goes away: still Fault until the operator resets.
    core.setInjected({});
    for (int i = 0; i < 20; ++i) {
        step();
    }
    EXPECT_EQ(status().state, StateKind::Fault);
    EXPECT_TRUE(status().resetAllowed);
    EXPECT_FALSE(arm.moving);

    core.request(Request::Reset);
    step();
    EXPECT_EQ(status().state, StateKind::Idle);  // not engaged: the operator takes over on purpose
    EXPECT_FALSE(arm.moving);
}

TEST_F(Fixture, REQ_SAF_002_ResetIsRefusedWhileTheArmStillDoesNotAcknowledge) {
    // Found by the fault injector: the commands stop reaching the arm (corrupted on the way),
    // the console faults, and the cause has to stay visible in Fault, where the console sends
    // holds that must be acknowledged just the same.
    engage();
    for (int i = 0; i < 25; ++i) {
        step(5ms, false);
        arm.answerWithoutAck(ArmState::Holding);
    }
    ASSERT_EQ(status().state, StateKind::Fault);
    core.request(Request::Reset);
    step(5ms, false);
    EXPECT_EQ(status().state, StateKind::Fault);
    EXPECT_FALSE(status().resetAllowed);
    // Acknowledgements come back: the reset goes through.
    step();
    EXPECT_TRUE(status().resetAllowed);
    core.request(Request::Reset);
    step();
    EXPECT_EQ(status().state, StateKind::Idle);
}

TEST_F(Fixture, ARequestIsHandledOnce) {
    engage();
    core.request(Request::Disengage);
    step();
    ASSERT_EQ(status().state, StateKind::Idle);
    // Ticking on must not replay the old disengage after a new engage.
    core.request(Request::Engage);
    step();
    ASSERT_EQ(status().state, StateKind::Engaged);
    for (int i = 0; i < 10; ++i) {
        step();
    }
    EXPECT_EQ(status().state, StateKind::Engaged);
}

}  // namespace
}  // namespace msc::teleop
