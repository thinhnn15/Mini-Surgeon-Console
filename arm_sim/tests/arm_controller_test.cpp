#include "msc/arm/arm_controller.hpp"

#include "msc/common/clock.hpp"
#include "msc/comms/heartbeat_monitor.hpp"
#include "msc/comms/protocol.hpp"
#include "msc/comms/udp_socket.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
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
using comms::ArmState;
using comms::Endpoint;
using comms::UdpSocket;

constexpr std::uint32_t kConsoleSession = 0xC0FFEE;

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

// The test's side of the link: a console that sends heartbeats and commands and reads what the
// arm sends back. One sequence counter for everything it sends, like the real console.
class FakeConsole {
public:
    void heartbeat(const Endpoint& arm) { send(arm, nextSequence_++, comms::Heartbeat{}); }
    void holdCommand(const Endpoint& arm) { send(arm, nextSequence_++, comms::HoldCommand{}); }
    [[nodiscard]] std::uint32_t lastSequence() const { return nextSequence_ - 1; }
    void command(const Endpoint& arm, const JointArray& target) {
        send(arm, nextSequence_++, comms::JointCommand{.target = target});
    }
    void send(const Endpoint& arm, std::uint32_t sequence, const comms::Message& message) {
        std::array<std::byte, comms::kMaxPacketSize> buffer{};
        const std::size_t size = comms::encode(comms::Packet{.session = kConsoleSession,
                                                             .sequence = sequence,
                                                             .timestamp = Nanoseconds{0},
                                                             .message = message},
                                               buffer);
        ASSERT_TRUE(socket_.sendTo(std::span(buffer).first(size), arm));
    }
    void sendRaw(const Endpoint& arm, std::span<const std::byte> bytes) {
        ASSERT_TRUE(socket_.sendTo(bytes, arm));
    }

    // Everything the arm sent since the last call: the newest telemetry, and how many heartbeats.
    struct Received {
        std::optional<comms::JointTelemetry> telemetry;
        int heartbeats = 0;
    };
    Received drain() {
        Received received;
        std::array<std::byte, comms::kMaxPacketSize> buffer{};
        for (;;) {
            const comms::ReceiveResult r = socket_.receive(buffer, 0ms);
            if (r.status != comms::ReceiveStatus::Datagram) {
                return received;
            }
            const comms::DecodeResult decoded = comms::decode(std::span(buffer).first(r.size));
            if (const auto* packet = std::get_if<comms::Packet>(&decoded)) {
                if (const auto* t = std::get_if<comms::JointTelemetry>(&packet->message)) {
                    received.telemetry = *t;
                } else if (std::holds_alternative<comms::Heartbeat>(packet->message)) {
                    ++received.heartbeats;
                }
            }
        }
    }
    [[nodiscard]] const Endpoint& endpoint() const { return socket_.localEndpoint(); }

private:
    UdpSocket socket_ = openLoopback();
    std::uint32_t nextSequence_ = 1;
};

// Ticks (1 ms of manual time each) until `done` holds, giving up after 2 s of real time.
// Loopback delivery is fast but not instantaneous, so "send, then one tick" is not enough.
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

// Sends a heartbeat and ticks until the arm has taken it in.
bool beat(ArmController& arm, ManualClock& clock, FakeConsole& console) {
    const auto before = arm.stats().heartbeatsReceived;
    console.heartbeat(arm.localEndpoint());
    return tickUntil(arm, clock, [&] { return arm.stats().heartbeatsReceived > before; });
}

// A console that is alive and has the arm following `target`.
void startFollowing(ArmController& arm, ManualClock& clock, FakeConsole& console, const JointArray& target) {
    ASSERT_TRUE(beat(arm, clock, console));
    const auto before = arm.stats().commandsApplied;
    console.command(arm.localEndpoint(), target);
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.stats().commandsApplied > before; }));
    ASSERT_EQ(arm.state(), ArmState::Following);
}

TEST(ArmController, IsIdleAndQuietUntilAConsoleTalksToIt) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    for (int i = 0; i < 10; ++i) {
        clock.advance(1ms);
        arm.tick();
    }
    EXPECT_EQ(arm.state(), ArmState::Idle);
    EXPECT_EQ(arm.consoleLink(), comms::LinkState::Waiting);
    EXPECT_EQ(arm.stats().telemetrySent, 0U);  // nobody to send to yet
    EXPECT_TRUE(arm.model().settled());
}

TEST(ArmController, FollowsACommandAndReportsBackToTheSender) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    startFollowing(arm, clock, console, all(0.5F));
    EXPECT_EQ(arm.model().target(), all(0.5F));

    // A second of manual time, with heartbeats: the joints get there, and the telemetry says so.
    for (int i = 0; i < 100; ++i) {
        ASSERT_TRUE(beat(arm, clock, console));
        for (int t = 0; t < 9; ++t) {
            clock.advance(1ms);
            arm.tick();
        }
    }
    std::optional<comms::JointTelemetry> report;
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return (report = console.drain().telemetry).has_value(); }));
    EXPECT_EQ(report->state, ArmState::Following);
    EXPECT_EQ(report->lastCommandSequence, 2U);  // heartbeat 1, command 2
    EXPECT_EQ(report->position, all(0.5F));
}

TEST(ArmController, SendsItsOwnHeartbeatEveryPeriod) {
    ManualClock clock;
    FakeConsole console;
    ArmController arm(clock, openLoopback(), ArmControllerConfig{.console = console.endpoint()});
    for (int i = 0; i < 100; ++i) {  // 100 ms
        clock.advance(1ms);
        arm.tick();
    }
    EXPECT_EQ(arm.stats().heartbeatsSent, 10U);
    int heartbeats = 0;
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return (heartbeats += console.drain().heartbeats) >= 10; }));
}

TEST(ArmController, TheNewestCommandWinsAndOlderOnesAreDropped) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    ASSERT_TRUE(beat(arm, clock, console));
    console.command(arm.localEndpoint(), all(0.1F));
    console.command(arm.localEndpoint(), all(0.2F));
    console.command(arm.localEndpoint(), all(0.3F));
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.consoleStats().accepted == 4; }));
    EXPECT_EQ(arm.model().target(), all(0.3F));
    // Whether the three arrived in one tick or several depends on the OS; either way the last
    // one is the target, and nothing was queued for later.
    EXPECT_EQ(arm.stats().commandsApplied + arm.stats().commandsReplaced, 3U);
}

TEST(ArmController, REQ_COM_001_AnOldCommandIsIgnored) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    console.send(arm.localEndpoint(), 9, comms::Heartbeat{});
    console.send(arm.localEndpoint(), 10, comms::JointCommand{.target = all(0.4F)});
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.stats().commandsApplied == 1; }));
    console.send(arm.localEndpoint(), 8, comms::JointCommand{.target = all(-0.4F)});  // delayed
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.stats().oldOrDuplicate == 1; }));
    EXPECT_EQ(arm.model().target(), all(0.4F));
}

TEST(ArmController, REQ_COM_001_ACorruptedDatagramIsCountedAndIgnored) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    ASSERT_TRUE(beat(arm, clock, console));
    std::array<std::byte, comms::kMaxPacketSize> buffer{};
    const std::size_t size = comms::encode(comms::Packet{.session = kConsoleSession,
                                                         .sequence = 100,
                                                         .timestamp = Nanoseconds{0},
                                                         .message = comms::JointCommand{.target = all(1.0F)}},
                                           buffer);
    buffer[30] ^= std::byte{0x04};  // one bit in the payload
    console.sendRaw(arm.localEndpoint(), std::span(buffer).first(size));
    ASSERT_TRUE(tickUntil(arm, clock, [&] {
        return arm.stats().refused[static_cast<std::size_t>(comms::DecodeError::BadCrc)] == 1;
    }));
    EXPECT_EQ(arm.state(), ArmState::Idle);
    EXPECT_EQ(arm.model().target(), all(0.0F));
}

TEST(ArmController, REQ_SAF_001_CommandsWithoutAHeartbeatAreIgnored) {
    // A command from something that never proved it is alive (or a stray packet) moves nothing.
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    console.command(arm.localEndpoint(), all(1.0F));
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.stats().commandsIgnored == 1; }));
    EXPECT_EQ(arm.state(), ArmState::Idle);
    EXPECT_EQ(arm.model().target(), all(0.0F));
}

TEST(ArmController, REQ_SAF_001_HoldsWithinThreeHeartbeatPeriodsOfSilence) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    startFollowing(arm, clock, console, all(2.0F));
    for (int i = 0; i < 20; ++i) {  // moving, console alive
        ASSERT_TRUE(beat(arm, clock, console));
        clock.advance(9ms);
        arm.tick();
        ASSERT_EQ(arm.state(), ArmState::Following);
    }
    ASSERT_TRUE(beat(arm, clock, console));
    const Nanoseconds lastHeartbeat = clock.now();

    // The console dies. The arm must notice once more than 3 periods (30 ms) have passed.
    while (arm.state() == ArmState::Following && clock.now() - lastHeartbeat < 1s) {
        clock.advance(1ms);
        arm.tick();
    }
    const Nanoseconds noticed = clock.now() - lastHeartbeat;
    EXPECT_EQ(arm.state(), ArmState::Holding);
    EXPECT_EQ(arm.consoleLink(), comms::LinkState::Lost);
    EXPECT_GT(noticed, 30ms);
    EXPECT_LE(noticed, 31ms);  // the first tick after the limit
    EXPECT_EQ(arm.stats().holds, 1U);
}

TEST(ArmController, REQ_SAF_001_HoldBrakesWithinTheLimitsAndStays) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    startFollowing(arm, clock, console, all(2.0F));
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(beat(arm, clock, console));
        clock.advance(9ms);
        arm.tick();
    }
    // Silence until it holds, then watch the braking tick by tick.
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.state() == ArmState::Holding; }));
    JointArray previous = arm.model().velocity();
    for (int i = 0; i < 2000; ++i) {
        clock.advance(1ms);
        arm.tick();
        const JointArray v = arm.model().velocity();
        for (std::size_t j = 0; j < kJointCount; ++j) {
            ASSERT_LE(std::abs(v[j] - previous[j]) / 0.001, arm.model().limits()[j].maxAcceleration * 1.001);
        }
        previous = v;
    }
    EXPECT_TRUE(arm.model().settled());
    const JointArray stoppedAt = arm.model().position();
    for (int i = 0; i < 500; ++i) {
        clock.advance(1ms);
        arm.tick();
    }
    EXPECT_EQ(arm.model().position(), stoppedAt);
    EXPECT_EQ(arm.state(), ArmState::Holding);
}

TEST(ArmController, REQ_SAF_001_WhenTheConsoleComesBackItWaitsForANewCommand) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    startFollowing(arm, clock, console, all(2.0F));
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.state() == ArmState::Holding; }));
    const JointArray heldTarget = arm.model().target();

    // Commands during the silence are ignored...
    console.command(arm.localEndpoint(), all(-1.0F));
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.stats().commandsIgnored == 1; }));
    // ...and heartbeats coming back do not resume the old move by themselves.
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(beat(arm, clock, console));
    }
    EXPECT_EQ(arm.consoleLink(), comms::LinkState::Alive);
    EXPECT_EQ(arm.state(), ArmState::Holding);
    EXPECT_EQ(arm.model().target(), heldTarget);

    const auto before = arm.stats().commandsApplied;
    console.command(arm.localEndpoint(), all(0.5F));
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.stats().commandsApplied > before; }));
    EXPECT_EQ(arm.state(), ArmState::Following);
    EXPECT_EQ(arm.model().target(), all(0.5F));
}

TEST(ArmController, AHoldCommandStopsTheArmWithTheLinkAlive) {
    ManualClock clock;
    ArmController arm(clock, openLoopback());
    FakeConsole console;
    startFollowing(arm, clock, console, all(2.0F));
    for (int i = 0; i < 20; ++i) {
        ASSERT_TRUE(beat(arm, clock, console));
        clock.advance(9ms);
        arm.tick();
    }
    console.holdCommand(arm.localEndpoint());
    const std::uint32_t holdSequence = console.lastSequence();
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.state() == ArmState::Holding; }));
    EXPECT_EQ(arm.consoleLink(), comms::LinkState::Alive);
    EXPECT_EQ(arm.stats().holds, 0U);  // not the watchdog: told to
    // The telemetry acknowledges the hold like any command.
    std::optional<comms::JointTelemetry> report;
    ASSERT_TRUE(tickUntil(arm, clock, [&] {
        const auto t = console.drain().telemetry;
        if (t) {
            report = t;
        }
        return report && report->lastCommandSequence == holdSequence;
    }));
    EXPECT_EQ(report->state, ArmState::Holding);

    // Braking at the limits, then staying; a new move command resumes.
    for (int i = 0; i < 250; ++i) {  // 2.5 s, heartbeats every 10 ms: the link stays alive
        ASSERT_TRUE(beat(arm, clock, console));
        clock.advance(9ms);
        arm.tick();
    }
    EXPECT_TRUE(arm.model().settled());
    EXPECT_EQ(arm.consoleLink(), comms::LinkState::Alive);
    console.command(arm.localEndpoint(), all(0.1F));
    ASSERT_TRUE(tickUntil(arm, clock, [&] { return arm.state() == ArmState::Following; }));
    EXPECT_EQ(arm.model().target(), all(0.1F));
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
