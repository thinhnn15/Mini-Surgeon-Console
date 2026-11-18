// The teleop core against the real arm controller, over real UDP on the loopback: every piece on
// its own thread, as in the running system (arm loop, arm link, teleop core), with the test
// thread playing the hand. Only the process boundary is missing (link_check covers that).

#include "msc/arm/arm_controller.hpp"
#include "msc/common/clock.hpp"
#include "msc/comms/arm_link.hpp"
#include "msc/comms/udp_socket.hpp"
#include "msc/teleop/teleop_core.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>

namespace msc::teleop {
namespace {

using namespace std::chrono_literals;
using Steady = std::chrono::steady_clock;

comms::UdpSocket openLoopback() {
    std::string error;
    std::optional<comms::UdpSocket> socket = comms::UdpSocket::open(comms::Endpoint::loopback(0), &error);
    if (!socket) {
        ADD_FAILURE() << "cannot open a loopback UDP socket: " << error;
        std::abort();
    }
    return std::move(*socket);
}

class TeleopWithRealArm : public ::testing::Test {
protected:
    TeleopWithRealArm() : arm_(clock_, openLoopback()), link_(clock_, openLoopback(), linkConfig()) {}

    void SetUp() override {
        armThread_ = std::jthread([this] { arm_.run(stopArm_); });
        link_.start();
        core_.start();
    }
    void TearDown() override {
        core_.stop();
        link_.stop();
        stopArm_ = true;
        armThread_.join();
    }

    comms::ArmLinkConfig linkConfig() { return comms::ArmLinkConfig{.arm = arm_.localEndpoint()}; }

    // Plays the hand every 5 ms until `done` holds for the newest status, or `timeout` passes.
    bool pump(Steady::duration timeout, const std::function<bool(const TeleopStatus&)>& done) {
        const auto giveUp = Steady::now() + timeout;
        while (Steady::now() < giveUp) {
            core_.submitHand(HandSample{.pose = hand_, .clutch = clutch_, .time = clock_.now()});
            static_cast<void>(core_.readStatus(status_));
            if (done(status_)) {
                return true;
            }
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }
    [[nodiscard]] float armJoint0() const { return status_.link.telemetry.position[0]; }

    SteadyClock clock_;
    arm::ArmController arm_;
    comms::ArmLink link_;
    TeleopCore core_{clock_, link_};
    std::atomic<bool> stopArm_{false};
    std::jthread armThread_;
    TeleopStatus status_;
    JointArray hand_{};
    bool clutch_ = false;
};

TEST_F(TeleopWithRealArm, REQ_SAF_003_TheArmFollowsTheHandScaledAndStaysPutWhileClutched) {
    ASSERT_TRUE(pump(3s, [](const TeleopStatus& s) { return s.engageAllowed; })) << "arm link never came up";
    core_.request(Request::Engage);
    ASSERT_TRUE(pump(1s, [](const TeleopStatus& s) { return s.state == StateKind::Engaged; }));

    // Hand +0.4 rad on joint 1: the arm goes +0.2 (scale 0.5) and settles there.
    hand_[0] = 0.4F;
    ASSERT_TRUE(pump(3s,
                     [this](const TeleopStatus& s) {
                         return std::abs(armJoint0() - 0.2F) < 1e-4F && s.link.telemetry.velocity[0] == 0.0F;
                     }))
        << "arm at " << armJoint0();
    EXPECT_EQ(status_.state, StateKind::Engaged);

    // Clutch, and bring the hand back past the start: the arm must not move.
    clutch_ = true;
    ASSERT_TRUE(pump(1s, [](const TeleopStatus& s) { return s.state == StateKind::Paused; }));
    hand_[0] = -0.4F;
    pump(500ms, [](const TeleopStatus&) { return false; });
    EXPECT_EQ(status_.state, StateKind::Paused);
    EXPECT_NEAR(armJoint0(), 0.2F, 1e-4F);

    // Release: re-indexed, the arm stays; new motion moves it from there.
    clutch_ = false;
    ASSERT_TRUE(pump(1s, [](const TeleopStatus& s) { return s.state == StateKind::Engaged; }));
    pump(300ms, [](const TeleopStatus&) { return false; });
    EXPECT_NEAR(armJoint0(), 0.2F, 1e-4F);
    hand_[0] = -0.2F;  // +0.2 of hand from the new anchor: arm +0.1
    ASSERT_TRUE(pump(3s, [this](const TeleopStatus&) { return std::abs(armJoint0() - 0.3F) < 1e-4F; }))
        << "arm at " << armJoint0();
}

TEST_F(TeleopWithRealArm, REQ_SAF_002_AFaultStopsTheRealArm) {
    ASSERT_TRUE(pump(3s, [](const TeleopStatus& s) { return s.engageAllowed; }));
    core_.request(Request::Engage);
    ASSERT_TRUE(pump(1s, [](const TeleopStatus& s) { return s.state == StateKind::Engaged; }));
    hand_[0] = 2.0F;  // a long move: arm toward +1.0
    ASSERT_TRUE(pump(2s, [](const TeleopStatus& s) { return s.link.telemetry.velocity[0] > 0.5F; }));

    core_.setInjected(faultSet(FaultCode::Injected));
    ASSERT_TRUE(pump(1s, [](const TeleopStatus& s) { return s.state == StateKind::Fault; }));
    // The core now sends holds: the arm brakes, stops short of the target and stays.
    ASSERT_TRUE(pump(2s, [](const TeleopStatus& s) {
        return s.link.telemetry.state == comms::ArmState::Holding && s.link.telemetry.velocity[0] == 0.0F;
    }));
    const float stoppedAt = armJoint0();
    EXPECT_LT(stoppedAt, 1.0F);
    pump(300ms, [](const TeleopStatus&) { return false; });
    EXPECT_EQ(armJoint0(), stoppedAt);
}

}  // namespace
}  // namespace msc::teleop
