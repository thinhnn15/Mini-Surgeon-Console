#include "msc/arm/arm_model.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>

namespace msc::arm {
namespace {

double sign(double value) noexcept {
    return static_cast<double>(static_cast<int>(value > 0.0) - static_cast<int>(value < 0.0));
}

}  // namespace

Limits defaultLimits() noexcept {
    return Limits{{
        {.min = -2.9, .max = 2.9, .maxVelocity = 1.5, .maxAcceleration = 6.0},   // base
        {.min = -1.9, .max = 1.9, .maxVelocity = 1.5, .maxAcceleration = 6.0},   // shoulder
        {.min = -2.5, .max = 2.5, .maxVelocity = 1.5, .maxAcceleration = 6.0},   // elbow
        {.min = -3.0, .max = 3.0, .maxVelocity = 2.5, .maxAcceleration = 10.0},  // wrist 1
        {.min = -2.0, .max = 2.0, .maxVelocity = 2.5, .maxAcceleration = 10.0},  // wrist 2
        {.min = -3.0, .max = 3.0, .maxVelocity = 2.5, .maxAcceleration = 10.0},  // wrist 3
    }};
}

ArmModel::ArmModel(const Limits& limits) noexcept : limits_(limits) {
    // Start at rest, at the middle of each range (0 for the default limits).
    for (std::size_t i = 0; i < kJointCount; ++i) {
        const double middle = (limits_[i].min + limits_[i].max) / 2.0;
        joints_[i] = Joint{.position = middle, .velocity = 0.0, .target = middle};
    }
}

void ArmModel::setTarget(const JointArray& target) noexcept {
    for (std::size_t i = 0; i < kJointCount; ++i) {
        joints_[i].target = std::clamp(static_cast<double>(target[i]), limits_[i].min, limits_[i].max);
    }
}

void ArmModel::step(Nanoseconds dt) noexcept {
    const double seconds = std::chrono::duration<double>(dt).count();
    if (seconds <= 0.0) {
        return;
    }
    for (std::size_t i = 0; i < kJointCount; ++i) {
        Joint& j = joints_[i];
        const JointLimits& limit = limits_[i];
        const double maxChange = limit.maxAcceleration * seconds;  // velocity change allowed this step

        const double error = j.target - j.position;
        // The fastest speed from which the joint can still stop at the target. Continuous
        // physics says v^2 = 2 a d, but the motion goes in steps: this step first moves v * dt,
        // and braking in steps of dt from v then covers v^2 / 2a - v dt / 2. Solving
        // v dt + v^2 / 2a - v dt / 2 <= d for v gives the speed below; with plain sqrt(2 a d)
        // the joints overshot by ~0.1 mrad and crawled back (measured, see the journal).
        const double a = limit.maxAcceleration;
        const double halfStep = a * seconds / 2.0;
        const double stoppable = std::sqrt((halfStep * halfStep) + (2.0 * a * std::abs(error))) - halfStep;
        const double wanted = sign(error) * std::min(limit.maxVelocity, stoppable);
        const double before = j.velocity;
        j.velocity += std::clamp(wanted - before, -maxChange, maxChange);
        j.position += j.velocity * seconds;

        // Arrived: the target is reached or passed by a hair within this step, from a speed
        // that one step's braking can take to zero. Stop there exactly, instead of hunting
        // around the target. Judged by the speed at the start of the step: going from that to
        // zero is one step's change at most, so the acceleration limit holds here too.
        const double after = j.target - j.position;
        const bool crossed = sign(after) != sign(error) || after == 0.0;
        if (crossed && std::abs(before) <= maxChange) {
            j.position = j.target;
            j.velocity = 0.0;
        }

        // Never outside the range, whatever happened above.
        if (j.position < limit.min || j.position > limit.max) {
            j.position = std::clamp(j.position, limit.min, limit.max);
            j.velocity = 0.0;
        }
    }
}

JointArray ArmModel::position() const noexcept {
    JointArray out{};
    for (std::size_t i = 0; i < kJointCount; ++i) {
        out[i] = static_cast<float>(joints_[i].position);
    }
    return out;
}

JointArray ArmModel::velocity() const noexcept {
    JointArray out{};
    for (std::size_t i = 0; i < kJointCount; ++i) {
        out[i] = static_cast<float>(joints_[i].velocity);
    }
    return out;
}

JointArray ArmModel::target() const noexcept {
    JointArray out{};
    for (std::size_t i = 0; i < kJointCount; ++i) {
        out[i] = static_cast<float>(joints_[i].target);
    }
    return out;
}

bool ArmModel::settled() const noexcept {
    return std::ranges::all_of(joints_,
                               [](const Joint& j) { return j.position == j.target && j.velocity == 0.0; });
}

}  // namespace msc::arm
