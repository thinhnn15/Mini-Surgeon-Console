#include "msc/arm/arm_controller.hpp"

#include "msc/common/clock.hpp"
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
#include <utility>
#include <variant>
#include <vector>

namespace msc::arm {
namespace {

using namespace std::chrono_literals;
using comms::Endpoint;
using comms::UdpSocket;

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

// The test's side of the link: a console that sends commands and reads telemetry.
class FakeConsole {
public:
    void sendCommand(const Endpoint& arm, std::uint32_t sequence, const JointArray& target,
                     std::uint32_t session = 0xC0FFEE) {
        send(arm, comms::Packet{.session = session,
                                .sequence = sequence,
                                .timestamp = Nanoseconds{0},
                                .message = comms::JointCommand{.target = target}});
    }
    void send(const Endpoint& arm, const comms::Packet& packet) {
        std::array<std::byte, comms::kMaxPacketSize> buffer{};
        const std::size_t size = comms::encode(packet, buffer);
        ASSERT_TRUE(socket_.sendTo(std::span(buffer).first(size), arm));
    }
    void sendRaw(const Endpoint& arm, std::span<const std::byte> bytes) {
        ASSERT_TRUE(socket_.sendTo(bytes, arm));
    }

    // The newest telemetry waiting, if any.
    std::optional<comms::JointTelemetry> newestTelemetry() {
        std::optional<comms::JointTelemetry> newest;
        std::array<std::byte, comms::kMaxPacketSize> buffer{};
        for (;;) {
            const comms::ReceiveResult r = socket_.receive(buffer, 0ms);
            if (r.status != comms::ReceiveStatus::Datagram) {
                return newest;
            }
            const comms::DecodeResult decoded = comms::decode(std::span(buffer).first(r.size));
            if (const auto* packet = std::get_if<comms::Packet>(&decoded)) {
                if (const auto* t = std::get_if<comms::JointTelemetry>(&packet->message)) {
                    newest = *t;
                }
            }
        }
    }
    [[nodiscard]] const Endpoint& endpoint() const { return socket_.localEndpoint(); }

private:
    UdpSocket socket_ = openLoopback();
};

// Loopback delivery is fast but not instantaneous: tick (1 ms of manual time each) until the
// condition holds, giving up after 2 s of real time.
bool tickUntil(ArmController& arm, ManualClock& clock, const std::function<bool()>& done) {
    const auto giveUp = std::chrono::steady_clock::now() + 2s;
    while (!done()) {
        if (std::chrono::steady_clock::now() > giveUp) {
            return false;
        }
        clock.advance(1ms);
        arm.tick();
    }
    return true;
}

TEST(ArmController, IsIdleAndQuietUntilTheFirstCommand) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    for (int i = 0; i < 10; ++i) {
        clock.advance(1ms);
        arm.tick();
    }
    EXPECT_EQ(arm.state(), comms::ArmState::Idle);
    EXPECT_EQ(arm.stats().telemetrySent, 0U);  // nobody to send to yet
    EXPECT_TRUE(arm.model().settled());
}

TEST(ArmController, FollowsACommandAndReportsBackToTheSender) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    console.sendCommand(arm.localEndpoint(), 1, all(0.5F));
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.stats().commandsApplied == 1; }));
    EXPECT_EQ(arm.state(), comms::ArmState::Following);
    EXPECT_EQ(arm.model().target(), all(0.5F));

    // A second of manual time: the joints get there, and the telemetry says so.
    for (int i = 0; i < 1000; ++i) {
        clock.advance(1ms);
        arm.tick();
    }
    std::optional<comms::JointTelemetry> report;
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return (report = console.newestTelemetry()).has_value(); }));
    EXPECT_EQ(report->state, comms::ArmState::Following);
    EXPECT_EQ(report->lastCommandSequence, 1U);
    EXPECT_EQ(report->position, all(0.5F));
}

TEST(ArmController, TheNewestCommandOfATickWinsAndOlderOnesAreDropped) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    console.sendCommand(arm.localEndpoint(), 1, all(0.1F));
    console.sendCommand(arm.localEndpoint(), 2, all(0.2F));
    console.sendCommand(arm.localEndpoint(), 3, all(0.3F));
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.commandStats().accepted == 3; }));
    EXPECT_EQ(arm.model().target(), all(0.3F));
    // Whether the three arrived in one tick or several depends on the OS; either way the last
    // one is the target, and nothing was queued for later.
    EXPECT_EQ(arm.stats().commandsApplied + arm.stats().commandsReplaced, 3U);
}

TEST(ArmController, REQ_COM_001_AnOldCommandIsIgnored) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    console.sendCommand(arm.localEndpoint(), 10, all(0.4F));
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.stats().commandsApplied == 1; }));
    console.sendCommand(arm.localEndpoint(), 9, all(-0.4F));  // delayed on the way
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.stats().oldOrDuplicate == 1; }));
    EXPECT_EQ(arm.model().target(), all(0.4F));
}

TEST(ArmController, REQ_COM_001_ACorruptedDatagramIsCountedAndIgnored) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    std::array<std::byte, comms::kMaxPacketSize> buffer{};
    const std::size_t size = comms::encode(comms::Packet{.session = 1,
                                                         .sequence = 1,
                                                         .timestamp = Nanoseconds{0},
                                                         .message = comms::JointCommand{.target = all(1.0F)}},
                                           buffer);
    buffer[30] ^= std::byte{0x04};  // one bit in the payload
    console.sendRaw(arm.localEndpoint(), std::span(buffer).first(size));
    ASSERT_TRUE(tickUntil(arm, clock, [&] {
        return arm.stats().refused[static_cast<std::size_t>(comms::DecodeError::BadCrc)] == 1;
    }));
    EXPECT_EQ(arm.state(), comms::ArmState::Idle);
    EXPECT_EQ(arm.model().target(), all(0.0F));
}

TEST(ArmController, TelemetryGoesToTheConfiguredConsole) {
    ManualClock clock;
    FakeConsole console;
    ArmController arm(clock, openLoopback(), ArmControllerConfig{.console = console.endpoint()});
    std::optional<comms::JointTelemetry> report;
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return (report = console.newestTelemetry()).has_value(); }));
    EXPECT_EQ(report->state, comms::ArmState::Idle);
}

TEST(ArmController, RunTicksAtItsPeriod) {
    // Real time: loose bounds, this is about "it runs and paces", not about precision (the
    // journal has the measured lateness).
    const SteadyClock clock;
    ArmController arm(clock, openLoopback());
    const std::atomic<bool> stop{false};
    arm.run(stop, 300ms);
    EXPECT_GE(arm.stats().ticks, 150U);
    EXPECT_LE(arm.stats().ticks, 305U);
    EXPECT_EQ(arm.lateness().count(), arm.stats().ticks);
}

TEST(ArmController, RunStopsWhenAsked) {
    const SteadyClock clock;
    ArmController arm(clock, openLoopback());
    const std::atomic<bool> stop{true};
    arm.run(stop);
    EXPECT_EQ(arm.stats().ticks, 0U);
}

}  // namespace
}  // namespace msc::arm
