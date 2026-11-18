#include "msc/comms/udp_socket.hpp"

#include "msc/comms/protocol.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace msc::comms {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

UdpSocket openLoopback() {
    std::string error;
    std::optional<UdpSocket> socket = UdpSocket::open(Endpoint::loopback(0), &error);
    if (!socket) {
        ADD_FAILURE() << "cannot open a loopback UDP socket: " << error;
        std::abort();
    }
    return std::move(*socket);
}

// A port that nobody listens on: bind one, remember it, close it again.
std::uint16_t unusedPort() {
    return openLoopback().localEndpoint().port;
}

TEST(Endpoint, ParsesAndPrints) {
    const std::optional<Endpoint> e = Endpoint::parse("192.168.1.20:47001");
    ASSERT_TRUE(e.has_value());
    EXPECT_EQ(*e, (Endpoint{{192, 168, 1, 20}, 47001}));
    EXPECT_EQ(e->toString(), "192.168.1.20:47001");
    EXPECT_EQ(Endpoint::loopback(5).toString(), "127.0.0.1:5");
}

TEST(Endpoint, RefusesAnythingElse) {
    for (const char* text : {"", "127.0.0.1", "127.0.0.1:", "127.0.0:1", "256.0.0.1:1", "127.0.0.1:65536",
                             "127.0.0.1:80x", "a.b.c.d:1", "127.0.0.1.5:1", " 127.0.0.1:1"}) {
        EXPECT_FALSE(Endpoint::parse(text).has_value()) << text;
    }
}

TEST(UdpSocket, PortZeroGetsARealPort) {
    const UdpSocket socket = openLoopback();
    EXPECT_NE(socket.localEndpoint().port, 0);
    EXPECT_EQ(socket.localEndpoint().address, (std::array<std::uint8_t, 4>{127, 0, 0, 1}));
}

TEST(UdpSocket, APortInUseCannotBeOpenedTwice) {
    // arm_sim relies on this: a second instance must fail to start, not share the port.
    const UdpSocket first = openLoopback();
    std::string error;
    EXPECT_FALSE(UdpSocket::open(first.localEndpoint(), &error).has_value());
    EXPECT_FALSE(error.empty());
}

TEST(UdpSocket, APacketGoesFromOneSocketToAnother) {
    UdpSocket sender = openLoopback();
    UdpSocket receiver = openLoopback();
    const Packet packet{.session = 9, .sequence = 1, .timestamp = Nanoseconds{5}, .message = JointCommand{}};
    std::array<std::byte, kMaxPacketSize> out{};
    const std::size_t size = encode(packet, out);
    ASSERT_TRUE(sender.sendTo(std::span(out).first(size), receiver.localEndpoint()));

    std::array<std::byte, kMaxPacketSize> in{};
    const ReceiveResult r = receiver.receive(in, 2s);
    ASSERT_EQ(r.status, ReceiveStatus::Datagram) << "error " << r.errorCode;
    EXPECT_FALSE(r.truncated);
    EXPECT_EQ(r.from, sender.localEndpoint());
    const DecodeResult decoded = decode(std::span(in).first(r.size));
    ASSERT_TRUE(std::holds_alternative<Packet>(decoded));
    EXPECT_EQ(std::get<Packet>(decoded), packet);
}

TEST(UdpSocket, ReceiveWaitsForTheTimeoutAndNoLonger) {
    UdpSocket socket = openLoopback();
    std::array<std::byte, 64> buffer{};
    const auto start = Clock::now();
    const ReceiveResult r = socket.receive(buffer, 100ms);
    const auto waited = Clock::now() - start;
    EXPECT_EQ(r.status, ReceiveStatus::Timeout);
    // The OS may wake a little early (timer granularity) and, on a busy CI machine, late.
    EXPECT_GE(waited, 80ms);
    EXPECT_LT(waited, 2s);
}

TEST(UdpSocket, ZeroTimeoutOnlyLooks) {
    UdpSocket socket = openLoopback();
    std::array<std::byte, 64> buffer{};
    const auto start = Clock::now();
    EXPECT_EQ(socket.receive(buffer, 0ms).status, ReceiveStatus::Timeout);
    EXPECT_LT(Clock::now() - start, 500ms);
}

TEST(UdpSocket, ADatagramLargerThanTheBufferIsReportedAsTruncated) {
    UdpSocket sender = openLoopback();
    UdpSocket receiver = openLoopback();
    const std::vector<std::byte> big(200, std::byte{0x5A});
    ASSERT_TRUE(sender.sendTo(big, receiver.localEndpoint()));
    std::array<std::byte, 64> small{};
    const ReceiveResult r = receiver.receive(small, 2s);
    ASSERT_EQ(r.status, ReceiveStatus::Datagram) << "error " << r.errorCode;
    EXPECT_TRUE(r.truncated);
    EXPECT_EQ(r.size, small.size());
}

TEST(UdpSocket, SendingToAPortNobodyListensOnDoesNotBreakReceiving) {
    // The console restarts: for a moment the arm sends telemetry to a port with nobody behind
    // it. Windows answers that with ICMP "port unreachable" and, unless told otherwise, fails
    // the socket's next receive with WSAECONNRESET. The link must not notice.
    UdpSocket socket = openLoopback();
    const std::array<std::byte, 4> ping{};
    ASSERT_TRUE(socket.sendTo(ping, Endpoint::loopback(unusedPort())));
    std::array<std::byte, 64> buffer{};
    const ReceiveResult afterUnreachable = socket.receive(buffer, 100ms);
    EXPECT_EQ(afterUnreachable.status, ReceiveStatus::Timeout) << "error " << afterUnreachable.errorCode;

    UdpSocket other = openLoopback();
    ASSERT_TRUE(other.sendTo(ping, socket.localEndpoint()));
    EXPECT_EQ(socket.receive(buffer, 2s).status, ReceiveStatus::Datagram);
}

TEST(UdpSocket, MovedFromSocketIsEmptyAndTheNewOneWorks) {
    UdpSocket a = openLoopback();
    const Endpoint where = a.localEndpoint();
    UdpSocket b = std::move(a);
    UdpSocket sender = openLoopback();
    const std::array<std::byte, 4> ping{};
    ASSERT_TRUE(sender.sendTo(ping, where));
    std::array<std::byte, 64> buffer{};
    EXPECT_EQ(b.receive(buffer, 2s).status, ReceiveStatus::Datagram);
}

}  // namespace
}  // namespace msc::comms
