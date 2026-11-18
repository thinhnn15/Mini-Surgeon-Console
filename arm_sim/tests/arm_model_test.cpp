#include "msc/arm/arm_model.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <cstddef>

namespace msc::arm {
namespace {

using namespace std::chrono_literals;

constexpr Nanoseconds kTick = 1ms;  // the controller's period
constexpr double kDt = 0.001;

JointArray all(float value) {
    JointArray a{};
    a.fill(value);
    return a;
}

// Steps until settled or `limit` passes; returns the time it took (or `limit`).
Nanoseconds runUntilSettled(ArmModel& arm, Nanoseconds limit) {
    Nanoseconds t{0};
    while (!arm.settled() && t < limit) {
        arm.step(kTick);
        t += kTick;
    }
    return t;
}

TEST(ArmModel, StartsAtRestInTheMiddleOfEachRange) {
    const ArmModel arm;
    EXPECT_TRUE(arm.settled());
    EXPECT_EQ(arm.position(), all(0.0F));
    EXPECT_EQ(arm.velocity(), all(0.0F));
}

TEST(ArmModel, ReachesTheTargetAndStopsThere) {
    ArmModel arm;
    arm.setTarget(all(1.0F));
    runUntilSettled(arm, 5s);
    ASSERT_TRUE(arm.settled());
    EXPECT_EQ(arm.position(), all(1.0F));
    EXPECT_EQ(arm.velocity(), all(0.0F));
}

TEST(ArmModel, TakesAsLongAsTheTrapezoidSays) {
    // Base joint, 1 rad at 1.5 rad/s and 6 rad/s^2: 0.25 s up, 0.25 s down (0.1875 rad each),
    // 0.625 rad at full speed in 0.4167 s: 0.9167 s in all. Stepping adds at most a few ms.
    Limits limits = defaultLimits();
    ArmModel arm(limits);
    JointArray target{};
    target[0] = 1.0F;
    arm.setTarget(target);
    const Nanoseconds took = runUntilSettled(arm, 5s);
    EXPECT_GE(took, 910ms);
    EXPECT_LE(took, 930ms);
}

TEST(ArmModel, NeverExceedsItsVelocityOrAccelerationLimit) {
    ArmModel arm;
    const Limits& limits = arm.limits();
    JointArray previous = arm.velocity();
    // A move, then a reversal halfway through, then another target: the hard cases.
    const auto check = [&](Nanoseconds duration) {
        for (Nanoseconds t{0}; t < duration; t += kTick) {
            arm.step(kTick);
            const JointArray v = arm.velocity();
            for (std::size_t i = 0; i < kJointCount; ++i) {
                ASSERT_LE(std::abs(v[i]), limits[i].maxVelocity * 1.0001) << "joint " << i;
                ASSERT_LE(std::abs(v[i] - previous[i]) / kDt, limits[i].maxAcceleration * 1.001)
                    << "joint " << i;
            }
            previous = v;
        }
    };
    arm.setTarget(all(1.5F));
    check(400ms);
    arm.setTarget(all(-1.5F));
    check(3s);
    arm.setTarget(all(0.2F));
    check(3s);
    EXPECT_TRUE(arm.settled());
}

TEST(ArmModel, DoesNotOvershootTheTarget) {
    ArmModel arm;
    arm.setTarget(all(0.8F));
    for (Nanoseconds t{0}; t < 3s; t += kTick) {
        arm.step(kTick);
        for (const float p : arm.position()) {
            ASSERT_LE(p, 0.8F + 1e-4F);
        }
    }
}

TEST(ArmModel, TargetOutsideTheRangeIsClampedToTheRange) {
    // The arm protects itself, whatever the console sends.
    ArmModel arm;
    arm.setTarget(all(100.0F));
    runUntilSettled(arm, 10s);
    ASSERT_TRUE(arm.settled());
    for (std::size_t i = 0; i < kJointCount; ++i) {
        EXPECT_FLOAT_EQ(arm.position()[i], static_cast<float>(arm.limits()[i].max)) << "joint " << i;
        EXPECT_FLOAT_EQ(arm.target()[i], static_cast<float>(arm.limits()[i].max)) << "joint " << i;
    }
}

TEST(ArmModel, ZeroOrNegativeTimeChangesNothing) {
    ArmModel arm;
    arm.setTarget(all(1.0F));
    arm.step(0ms);
    arm.step(-5ms);
    EXPECT_EQ(arm.position(), all(0.0F));
    EXPECT_EQ(arm.velocity(), all(0.0F));
}

TEST(ArmModel, ALongStepStaysWithinTheLimits) {
    // A stalled loop (debugger, overloaded machine) may hand in a big dt. The speed may only
    // change by maxAcceleration * dt, and the range still holds.
    ArmModel arm;
    arm.setTarget(all(2.0F));
    arm.step(200ms);
    for (std::size_t i = 0; i < kJointCount; ++i) {
        EXPECT_LE(std::abs(arm.velocity()[i]), arm.limits()[i].maxVelocity * 1.0001);
        EXPECT_LE(std::abs(arm.position()[i]), arm.limits()[i].max);
    }
}

}  // namespace
}  // namespace msc::arm
