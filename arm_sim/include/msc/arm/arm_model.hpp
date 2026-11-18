#pragma once

#include "msc/common/clock.hpp"
#include "msc/comms/protocol.hpp"

#include <array>

namespace msc::arm {

using comms::JointArray;
using comms::kJointCount;

struct JointLimits {
    double min;              // rad
    double max;              // rad
    double maxVelocity;      // rad/s
    double maxAcceleration;  // rad/s^2
};
using Limits = std::array<JointLimits, kJointCount>;

// Limits of a small 6-joint arm: big shoulder joints slower than the wrist.
[[nodiscard]] Limits defaultLimits() noexcept;

// The simulated arm's joints: plain C++, no I/O and no clock of its own, so every rule is
// tested with plain numbers.
//
// Each joint moves toward its target with a trapezoidal velocity profile: speed up at most at
// maxAcceleration, never faster than maxVelocity, and start braking in time to stop at the
// target (from speed v, stopping takes v^2 / 2a of travel). A new target can come at any time,
// even mid-move: the joint brakes and turns around within its limits, it never jumps.
//
// The arm enforces its own limits whatever the console asks: a target outside a joint's range
// is clamped to the range. The console's checks are a convenience; these are the ones that
// protect the hardware.
class ArmModel {
public:
    explicit ArmModel(const Limits& limits = defaultLimits()) noexcept;

    // Clamped into each joint's range.
    void setTarget(const JointArray& target) noexcept;
    // Advances the motion by `dt`.
    void step(Nanoseconds dt) noexcept;

    [[nodiscard]] JointArray position() const noexcept;
    [[nodiscard]] JointArray velocity() const noexcept;
    [[nodiscard]] JointArray target() const noexcept;
    [[nodiscard]] const Limits& limits() const noexcept { return limits_; }
    // True when every joint is at its target and at rest.
    [[nodiscard]] bool settled() const noexcept;

private:
    struct Joint {
        double position = 0.0;
        double velocity = 0.0;
        double target = 0.0;
    };

    Limits limits_;
    std::array<Joint, kJointCount> joints_{};
};

}  // namespace msc::arm
