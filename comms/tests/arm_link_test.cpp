#include "msc/comms/arm_link.hpp"

#include "msc/common/clock.hpp"
#include "msc/common/spsc_queue.hpp"
#include "msc/comms/protocol.hpp"
#include "msc/comms/udp_socket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace msc::comms {
namespace {

using namespace std::chrono_literals;
using Steady = std::chrono::steady_clock;

UdpSocket openLoopback() {
    std::string error;
    std::optional<UdpSocket> socket = UdpSocket::open(Endpoint::loopback(0), &error);
    if (!socket) {
        ADD_FAILURE() << "cannot open a loopback UDP socket: " << error;
        std::abort();
    }
    return std::move(*socket);
}

JointArray all(float value) {
    JointArray a{};
    a.fill(value);
    return a;
}

// The test plays the arm: a bare socket that answers like arm_sim would.
class FakeArm {
public:
    void send(const Endpoint& console, const Message& message) {
        std::array<std::byte, kMaxPacketSize> buffer{};
        const std::size_t size = encode(Packet{.session = 0xA2A2,
                                               .sequence = nextSequence_++,
                                               .timestamp = Nanoseconds{0},
                                               .message = message},
                                        buffer);
        ASSERT_TRUE(socket_.sendTo(std::span(buffer).first(size), console));
    }
    // Waits up to `timeout` for a packet from the console.
    std::optional<Packet> receive(std::chrono::milliseconds timeout) {
        std::array<std::byte, kMaxPacketSize> buffer{};
        const ReceiveResult r = socket_.receive(buffer, timeout);
        if (r.status != ReceiveStatus::Datagram) {
            return std::nullopt;
        }
        const DecodeResult decoded = decode(std::span(buffer).first(r.size));
        if (!std::holds_alternative<Packet>(decoded)) {
            return std::nullopt;
        }
        return std::get<Packet>(decoded);
    }
    [[nodiscard]] const Endpoint& endpoint() const { return socket_.localEndpoint(); }

private:
    UdpSocket socket_ = openLoopback();
    std::uint32_t nextSequence_ = 1;
};

// Reads the link's status until `done` holds or `timeout` passes. Returns the last status.
ArmLinkStatus waitForStatus(ArmLink& link, const std::function<bool(const ArmLinkStatus&)>& done,
                            std::chrono::milliseconds timeout) {
    ArmLinkStatus status;
    const auto giveUp = Steady::now() + timeout;
    while (Steady::now() < giveUp) {
        static_cast<void>(link.readStatus(status));
        if (done(status)) {
            break;
        }
        std::this_thread::sleep_for(1ms);
    }
    return status;
}

TEST(ArmLink, SendsHeartbeatsAtItsPeriod) {
    const SteadyClock clock;
    FakeArm arm;
    ArmLink link(clock, openLoopback(), ArmLinkConfig{.arm = arm.endpoint()});
    link.start();
    int heartbeats = 0;
    const auto start = Steady::now();
    while (Steady::now() - start < 300ms) {
        if (const auto packet = arm.receive(50ms)) {
            heartbeats += std::holds_alternative<Heartbeat>(packet->message) ? 1 : 0;
        }
    }
    link.stop();
    // 10 ms period: 30 in 300 ms. Loose bounds, real time on a shared machine.
    EXPECT_GE(heartbeats, 20);
    EXPECT_LE(heartbeats, 35);
}

TEST(ArmLink, SendsTheNewestTargetAsACommand) {
    const SteadyClock clock;
    FakeArm arm;
    ArmLink link(clock, openLoopback(), ArmLinkConfig{.arm = arm.endpoint()});
    link.start();
    link.move(all(0.1F));
    link.move(all(0.7F));
    std::optional<JointCommand> command;
    const auto giveUp = Steady::now() + 2s;
    while (Steady::now() < giveUp && !(command && command->target == all(0.7F))) {
        if (const auto packet = arm.receive(50ms)) {
            if (const auto* c = std::get_if<JointCommand>(&packet->message)) {
                command = *c;
            }
        }
    }
    ASSERT_TRUE(command.has_value());
    EXPECT_EQ(command->target, all(0.7F));
}

TEST(ArmLink, REQ_SAF_001_ArmIsAliveWhileItsHeartbeatsComeAndLostWhenTheyStop) {
    const SteadyClock clock;
    FakeArm arm;
    ArmLink link(clock, openLoopback(), ArmLinkConfig{.arm = arm.endpoint()});
    link.start();
    EXPECT_EQ(waitForStatus(link, [](const ArmLinkStatus&) { return false; }, 50ms).link, LinkState::Waiting);

    // The arm answers every 10 ms for a while...
    const auto start = Steady::now();
    while (Steady::now() - start < 200ms) {
        arm.send(link.localEndpoint(), Heartbeat{});
        std::this_thread::sleep_for(10ms);
    }
    const ArmLinkStatus alive =
        waitForStatus(link, [](const ArmLinkStatus& s) { return s.link == LinkState::Alive; }, 1s);
    EXPECT_EQ(alive.link, LinkState::Alive);
    EXPECT_GE(alive.heartbeatsReceived, 10U);

    // ...then goes silent: lost after 3 periods, seen within one more status period.
    const ArmLinkStatus lost =
        waitForStatus(link, [](const ArmLinkStatus& s) { return s.link == LinkState::Lost; }, 1s);
    EXPECT_EQ(lost.link, LinkState::Lost);
    EXPECT_GT(lost.silence, 30ms);
}

TEST(ArmLink, ReportsTheArmsTelemetry) {
    const SteadyClock clock;
    FakeArm arm;
    ArmLink link(clock, openLoopback(), ArmLinkConfig{.arm = arm.endpoint()});
    link.start();
    const JointTelemetry sent{
        .position = all(0.25F), .velocity = all(0.0F), .state = ArmState::Holding, .lastCommandSequence = 12};
    arm.send(link.localEndpoint(), sent);
    const ArmLinkStatus status =
        waitForStatus(link, [](const ArmLinkStatus& s) { return s.telemetrySeen; }, 2s);
    ASSERT_TRUE(status.telemetrySeen);
    EXPECT_EQ(status.telemetry, sent);
    EXPECT_EQ(status.telemetryReceived, 1U);
}

// M5.1: whoever needs every telemetry sample (the overlay's pose history) gets each one, in
// order, with the time it arrived on the console's clock. The sink runs on the link thread, so
// it hands over through a lock-free queue, as the console does.
TEST(ArmLink, HandsEveryTelemetryPacketToTheSinkWithItsArrivalTime) {
    const SteadyClock clock;
    FakeArm arm;
    SpscQueue<TelemetrySample> received(64);
    ArmLink link(
        clock, openLoopback(),
        ArmLinkConfig{.arm = arm.endpoint(), .onTelemetry = [&received](const TelemetrySample& sample) {
                          static_cast<void>(received.tryPush(sample));
                      }});
    link.start();

    const Nanoseconds before = clock.now();
    constexpr int kPackets = 5;
    for (int i = 1; i <= kPackets; ++i) {
        arm.send(link.localEndpoint(), JointTelemetry{.position = all(0.1F * static_cast<float>(i)),
                                                      .velocity = all(0.0F),
                                                      .state = ArmState::Following,
                                                      .lastCommandSequence = 0});
        std::this_thread::sleep_for(2ms);
    }
    // Heartbeats in between must not reach the sink.
    arm.send(link.localEndpoint(), Heartbeat{});

    std::vector<TelemetrySample> samples;
    const auto giveUp = Steady::now() + 2s;
    while (Steady::now() < giveUp && samples.size() < kPackets) {
        TelemetrySample sample;
        if (received.tryPop(sample)) {
            samples.push_back(sample);
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    std::this_thread::sleep_for(30ms);  // anything that should not have come would be here by now
    const Nanoseconds after = clock.now();
    link.stop();

    ASSERT_EQ(samples.size(), static_cast<std::size_t>(kPackets));
    TelemetrySample extra;
    EXPECT_FALSE(received.tryPop(extra)) << "the heartbeat, or a packet twice, reached the sink";
    Nanoseconds previous = before;
    for (int i = 0; i < kPackets; ++i) {
        const TelemetrySample& sample = samples[static_cast<std::size_t>(i)];
        EXPECT_EQ(sample.telemetry.position, all(0.1F * static_cast<float>(i + 1))) << "packet " << i + 1;
        EXPECT_GE(sample.receivedAt, previous) << "arrival times must not go backwards";
        EXPECT_LE(sample.receivedAt, after);
        previous = sample.receivedAt;
    }
}

// The cable-pulled fault throws away everything that arrives: the sink must see none of it, or
// the overlay would keep following an arm the console no longer hears.
TEST(ArmLink, WithTheCablePulledNothingReachesTheSink) {
    const SteadyClock clock;
    FakeArm arm;
    std::atomic<int> calls{0};
    ArmLink link(clock, openLoopback(),
                 ArmLinkConfig{.arm = arm.endpoint(), .onTelemetry = [&calls](const TelemetrySample&) {
                                   calls.fetch_add(1, std::memory_order_relaxed);
                               }});
    link.setFault(LinkFault::Drop);
    link.start();
    for (int i = 0; i < 5; ++i) {
        arm.send(link.localEndpoint(), JointTelemetry{});
        std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(30ms);
    EXPECT_EQ(calls.load(), 0);

    // Plugged back in: the next packet arrives.
    link.setFault(LinkFault::None);
    const auto giveUp = Steady::now() + 2s;
    while (Steady::now() < giveUp && calls.load() == 0) {
        arm.send(link.localEndpoint(), JointTelemetry{});
        std::this_thread::sleep_for(5ms);
    }
    link.stop();
    EXPECT_GT(calls.load(), 0);
}

TEST(ArmLink, StopEndsTheHeartbeats) {
    // Stopping the link is what the arm sees when the console dies: silence.
    const SteadyClock clock;
    FakeArm arm;
    ArmLink link(clock, openLoopback(), ArmLinkConfig{.arm = arm.endpoint()});
    link.start();
    ASSERT_TRUE(arm.receive(1s).has_value());
    link.stop();
    while (arm.receive(0ms)) {
        // drain what was sent before the stop
    }
    EXPECT_FALSE(arm.receive(100ms).has_value());
}

}  // namespace
}  // namespace msc::comms
