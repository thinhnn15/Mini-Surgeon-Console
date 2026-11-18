#include "msc/teleop/state_machine.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <optional>
#include <variant>

namespace msc::teleop {
namespace {

using namespace std::chrono_literals;

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

constexpr Nanoseconds kNow{5s};

// Scaled targets go through float arithmetic: compare to within a micro-radian.
void expectNear(const std::optional<JointArray>& actual, const JointArray& expected) {
    ASSERT_TRUE(actual.has_value());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_NEAR((*actual)[i], expected[i], 1e-6F) << "joint " << i;
    }
}

// A machine that is engaged with the hand at 0 and the arm at 1 (scale 0.5).
StateMachine engaged() {
    StateMachine m(0.5);
    EXPECT_EQ(m.handle(Engage{.hand = all(0.0F), .arm = all(1.0F)}, kNow).outcome, Outcome::Changed);
    return m;
}

TEST(StateMachine, StartsIdleAndCommandsNothing) {
    const StateMachine m;
    EXPECT_TRUE(std::holds_alternative<Idle>(m.state()));
    EXPECT_FALSE(m.target(all(0.3F)).has_value());
    EXPECT_EQ(stateName(m.state()), "idle");
}

TEST(StateMachine, EngagingDoesNotMoveTheArm) {
    // Wherever the hand is, taking over keeps the arm where it is: the first target is the arm's
    // own position.
    StateMachine m(0.5);
    ASSERT_EQ(m.handle(Engage{.hand = all(0.7F), .arm = all(-0.2F)}, kNow).outcome, Outcome::Changed);
    EXPECT_TRUE(std::holds_alternative<Engaged>(m.state()));
    EXPECT_EQ(m.target(all(0.7F)), all(-0.2F));
}

TEST(StateMachine, HandMotionIsScaledOntoTheArm) {
    const StateMachine m = engaged();
    expectNear(m.target(first(0.4F)), JointArray{1.2F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F});
    expectNear(m.target(first(-0.4F)), JointArray{0.8F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F});
}

TEST(StateMachine, REQ_SAF_003_WhilePausedHandMotionCommandsNothing) {
    StateMachine m = engaged();
    ASSERT_EQ(m.handle(ClutchPressed{}, kNow).outcome, Outcome::Changed);
    EXPECT_TRUE(std::holds_alternative<Paused>(m.state()));
    for (const float hand : {0.0F, 0.5F, -1.0F, 3.0F}) {
        EXPECT_FALSE(m.target(all(hand)).has_value()) << hand;
    }
}

TEST(StateMachine, REQ_SAF_003_ClutchReleaseReindexesSoTheArmNeverJumps) {
    // Move the hand +0.4 (arm +0.2), clutch, bring the hand back to 0, release: the arm stays at
    // +0.2 and only new motion moves it.
    StateMachine m = engaged();
    expectNear(m.target(first(0.4F)), JointArray{1.2F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F});
    ASSERT_EQ(m.handle(ClutchPressed{}, kNow).outcome, Outcome::Changed);
    const JointArray armWhereItStopped{1.2F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F};
    ASSERT_EQ(m.handle(ClutchReleased{.hand = all(0.0F), .arm = armWhereItStopped}, kNow).outcome,
              Outcome::Changed);
    EXPECT_EQ(m.target(all(0.0F)), armWhereItStopped);
    expectNear(m.target(first(0.2F)), JointArray{1.3F, 1.0F, 1.0F, 1.0F, 1.0F, 1.0F});
}

TEST(StateMachine, DisengageFromEngagedOrPausedGoesIdle) {
    StateMachine a = engaged();
    EXPECT_EQ(a.handle(Disengage{}, kNow).outcome, Outcome::Changed);
    EXPECT_TRUE(std::holds_alternative<Idle>(a.state()));

    StateMachine b = engaged();
    ASSERT_EQ(b.handle(ClutchPressed{}, kNow).outcome, Outcome::Changed);
    EXPECT_EQ(b.handle(Disengage{}, kNow).outcome, Outcome::Changed);
    EXPECT_TRUE(std::holds_alternative<Idle>(b.state()));
}

TEST(StateMachine, EngageIsRefusedWhileAFaultConditionIsPresent) {
    StateMachine m;
    const Result r = m.handle(Engage{.present = faultSet(FaultCode::ArmLinkLost)}, kNow);
    EXPECT_EQ(r.outcome, Outcome::Refused);
    EXPECT_FALSE(r.why.empty());
    EXPECT_TRUE(std::holds_alternative<Idle>(m.state()));
}

TEST(StateMachine, REQ_SAF_002_AnyFaultMovesToFaultFromEveryState) {
    const auto check = [](StateMachine m) {
        const Result r = m.handle(FaultDetected{.code = FaultCode::HandInputStale}, Nanoseconds{7s});
        EXPECT_EQ(r.outcome, Outcome::Changed);
        const auto* fault = std::get_if<Fault>(&m.state());
        ASSERT_NE(fault, nullptr);
        EXPECT_EQ(fault->code, FaultCode::HandInputStale);
        EXPECT_EQ(fault->since, Nanoseconds{7s});
        EXPECT_FALSE(m.target(all(0.1F)).has_value());
    };
    check(StateMachine{});  // idle
    check(engaged());
    StateMachine paused = engaged();
    ASSERT_EQ(paused.handle(ClutchPressed{}, kNow).outcome, Outcome::Changed);
    check(paused);
}

TEST(StateMachine, REQ_SAF_002_FaultIsLatchedAndIgnoresEverythingButReset) {
    StateMachine m = engaged();
    ASSERT_EQ(m.handle(FaultDetected{.code = FaultCode::ArmLinkLost}, kNow).outcome, Outcome::Changed);
    EXPECT_EQ(m.handle(Engage{}, kNow).outcome, Outcome::Refused);
    EXPECT_EQ(m.handle(Disengage{}, kNow).outcome, Outcome::Unchanged);
    EXPECT_EQ(m.handle(ClutchPressed{}, kNow).outcome, Outcome::Unchanged);
    EXPECT_EQ(m.handle(ClutchReleased{}, kNow).outcome, Outcome::Unchanged);
    // A second fault does not overwrite the first: the first is the cause.
    EXPECT_EQ(m.handle(FaultDetected{.code = FaultCode::VideoFrozen}, kNow + 1s).outcome, Outcome::Unchanged);
    const auto* fault = std::get_if<Fault>(&m.state());
    ASSERT_NE(fault, nullptr);
    EXPECT_EQ(fault->code, FaultCode::ArmLinkLost);
    EXPECT_EQ(fault->since, kNow);
}

TEST(StateMachine, REQ_SAF_002_ResetIsRefusedWhileTheCauseIsPresent) {
    StateMachine m = engaged();
    ASSERT_EQ(m.handle(FaultDetected{.code = FaultCode::ArmLinkLost}, kNow).outcome, Outcome::Changed);
    const Result refused = m.handle(Reset{.present = faultSet(FaultCode::ArmLinkLost)}, kNow);
    EXPECT_EQ(refused.outcome, Outcome::Refused);
    EXPECT_FALSE(refused.why.empty());
    EXPECT_TRUE(std::holds_alternative<Fault>(m.state()));
}

TEST(StateMachine, REQ_SAF_002_ResetAfterTheCauseIsGoneGoesIdleNotEngaged) {
    // Recovery is never automatic: the cause going away changes nothing until the operator
    // resets, and after the reset the operator still has to engage on purpose.
    StateMachine m = engaged();
    ASSERT_EQ(m.handle(FaultDetected{.code = FaultCode::ArmLinkLost}, kNow).outcome, Outcome::Changed);
    EXPECT_EQ(m.handle(Reset{.present = {}}, kNow).outcome, Outcome::Changed);
    EXPECT_TRUE(std::holds_alternative<Idle>(m.state()));
    EXPECT_FALSE(m.target(all(0.0F)).has_value());
}

TEST(StateMachine, EventsThatMeanNothingInAStateChangeNothing) {
    StateMachine idle;
    EXPECT_EQ(idle.handle(ClutchPressed{}, kNow).outcome, Outcome::Unchanged);
    EXPECT_EQ(idle.handle(ClutchReleased{}, kNow).outcome, Outcome::Unchanged);
    EXPECT_EQ(idle.handle(Disengage{}, kNow).outcome, Outcome::Unchanged);
    EXPECT_EQ(idle.handle(Reset{}, kNow).outcome, Outcome::Unchanged);
    EXPECT_TRUE(std::holds_alternative<Idle>(idle.state()));

    StateMachine m = engaged();
    EXPECT_EQ(m.handle(Engage{.hand = all(5.0F), .arm = all(5.0F)}, kNow).outcome, Outcome::Unchanged);
    EXPECT_EQ(m.target(all(0.0F)), all(1.0F));  // anchors untouched
    EXPECT_EQ(m.handle(ClutchReleased{}, kNow).outcome, Outcome::Unchanged);
    EXPECT_EQ(m.handle(Reset{}, kNow).outcome, Outcome::Unchanged);
}

TEST(FaultSet, FirstFaultAndNames) {
    FaultSet set;
    EXPECT_FALSE(firstFault(set).has_value());
    set |= faultSet(FaultCode::VideoFrozen);
    set |= faultSet(FaultCode::ArmLinkLost);
    EXPECT_EQ(firstFault(set), FaultCode::ArmLinkLost);
    for (std::size_t i = 0; i < kFaultCodeCount; ++i) {
        EXPECT_NE(toString(static_cast<FaultCode>(i)), "unknown fault") << i;
    }
}

}  // namespace
}  // namespace msc::teleop
