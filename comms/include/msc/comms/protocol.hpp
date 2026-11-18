#pragma once

#include "msc/common/clock.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>

namespace msc::comms {

// The arm link's wire format (REQ-COM-001): what one UDP datagram between console_app and
// arm_sim contains. Plain C++, no sockets: encode() and decode() work on byte buffers, so
// every rule here is tested without a network.
//
// Layout, all integers little-endian, written byte by byte (never a struct copied from
// memory: padding and byte order are up to the compiler and the CPU, the wire is not):
//
//   offset  size  field
//        0     4  magic "MSCL"        not one of ours: refuse before reading anything else
//        4     1  version (1)         a peer with another layout: refuse, do not guess
//        5     1  message type
//        6     2  payload length      must match the datagram size exactly
//        8     4  session id          random per process start, see SequenceFilter
//       12     4  sequence number     +1 per packet sent in this session
//       16     8  timestamp           sender's monotonic clock, ns (not comparable with ours)
//       24     N  payload
//     24+N     4  CRC-32 of bytes 0 .. 24+N-1
//
// A CRC catches corruption (every single-bit error, every burst up to 32 bits, nearly all
// others). It is not a security check: anyone can compute a valid CRC for a forged packet.

inline constexpr std::size_t kJointCount = 6;
using JointArray = std::array<float, kJointCount>;  // radians, or radians per second

enum class MessageType : std::uint8_t {
    Heartbeat = 1,
    JointCommand = 2,
    JointTelemetry = 3,
    HoldCommand = 4,  // added in M4.2; a peer without it refuses it as an unknown type
};

// What the arm is doing, reported in its telemetry.
enum class ArmState : std::uint8_t {
    Idle = 0,       // no command received yet
    Following = 1,  // moving toward the newest commanded target
    Holding = 2,    // told to hold, or the console went silent (REQ-SAF-001): braking, then staying
};

// "I am alive." Both sides send it at a fixed rate; the content is the header itself.
struct Heartbeat {
    friend bool operator==(const Heartbeat&, const Heartbeat&) = default;
};

// Console -> arm: where each joint should go.
struct JointCommand {
    JointArray target{};
    friend bool operator==(const JointCommand&, const JointCommand&) = default;
};

// Console -> arm: stop where braking at the limits gets you, and stay, until the next
// JointCommand. Sent while nobody drives the arm (console idle, clutch held, fault), so the arm
// never keeps going toward a target the operator no longer controls.
struct HoldCommand {
    friend bool operator==(const HoldCommand&, const HoldCommand&) = default;
};

// Arm -> console: where the joints are, and what the arm is doing.
struct JointTelemetry {
    JointArray position{};
    JointArray velocity{};
    ArmState state = ArmState::Idle;
    std::uint32_t lastCommandSequence = 0;  // newest command (move or hold) applied: the console's ack
    friend bool operator==(const JointTelemetry&, const JointTelemetry&) = default;
};

using Message = std::variant<Heartbeat, JointCommand, JointTelemetry, HoldCommand>;

struct Packet {
    std::uint32_t session = 0;
    std::uint32_t sequence = 0;
    Nanoseconds timestamp{0};
    Message message{};
    friend bool operator==(const Packet&, const Packet&) = default;
};

inline constexpr std::array<std::byte, 4> kMagic{std::byte{'M'}, std::byte{'S'}, std::byte{'C'},
                                                 std::byte{'L'}};
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kHeaderSize = 24;
inline constexpr std::size_t kCrcSize = 4;
// Larger than any packet of version 1 (the telemetry is 81 bytes); a receive buffer of this
// size always holds one, and anything longer is not ours.
inline constexpr std::size_t kMaxPacketSize = 128;

// Bytes encode() writes for this message: header + payload + CRC.
[[nodiscard]] std::size_t encodedSize(const Message& message) noexcept;

// Writes the packet into `out` and returns the number of bytes written, or 0 (and writes
// nothing) if `out` is too small.
[[nodiscard]] std::size_t encode(const Packet& packet, std::span<std::byte> out) noexcept;

// Why decode() refused a datagram. The order is the order of the checks.
enum class DecodeError : std::uint8_t {
    TooShort,        // shorter than an empty packet
    BadMagic,        // not our protocol
    BadVersion,      // our protocol, another version
    BadLength,       // payload length does not match the datagram size
    BadCrc,          // corrupted
    UnknownType,     // intact, but a message type we do not know
    BadPayloadSize,  // intact, known type, wrong size for it
    BadValue,        // intact and well formed, but a value no sender may send (NaN, unknown state)
};

[[nodiscard]] const char* toString(DecodeError error) noexcept;

using DecodeResult = std::variant<Packet, DecodeError>;

// The packet in `bytes` (one whole datagram), or why it was refused. Never reads outside
// `bytes`, whatever they contain. Ordering (old or duplicate packets) is not checked here:
// that needs memory of earlier packets, see SequenceFilter.
[[nodiscard]] DecodeResult decode(std::span<const std::byte> bytes) noexcept;

}  // namespace msc::comms
