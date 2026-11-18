#include "msc/comms/protocol.hpp"

#include "msc/common/crc32.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <variant>

namespace msc::comms {
namespace {

constexpr std::size_t kHeartbeatPayload = 0;
constexpr std::size_t kCommandPayload = kJointCount * 4;
constexpr std::size_t kTelemetryPayload = (2 * kJointCount * 4) + 1 + 4;

// Writes little-endian values one byte at a time into a buffer known to be big enough.
class Writer {
public:
    explicit Writer(std::span<std::byte> out) noexcept : out_(out) {}

    void u8(std::uint8_t value) noexcept { out_[pos_++] = static_cast<std::byte>(value); }
    void u16(std::uint16_t value) noexcept { little(value, 2); }
    void u32(std::uint32_t value) noexcept { little(value, 4); }
    void u64(std::uint64_t value) noexcept { little(value, 8); }
    // IEEE 754 single precision, by its bit pattern (every platform we build for uses it).
    void f32(float value) noexcept { u32(std::bit_cast<std::uint32_t>(value)); }
    void bytes(std::span<const std::byte> data) noexcept {
        std::ranges::copy(data, out_.subspan(pos_).begin());
        pos_ += data.size();
    }
    [[nodiscard]] std::size_t position() const noexcept { return pos_; }

private:
    void little(std::uint64_t value, int count) noexcept {
        for (int i = 0; i < count; ++i) {
            u8(static_cast<std::uint8_t>(value >> (8 * i)));
        }
    }

    std::span<std::byte> out_;
    std::size_t pos_ = 0;
};

// Reads little-endian values from a buffer whose size the caller has already checked.
class Reader {
public:
    explicit Reader(std::span<const std::byte> in) noexcept : in_(in) {}

    std::uint8_t u8() noexcept { return std::to_integer<std::uint8_t>(in_[pos_++]); }
    std::uint16_t u16() noexcept { return static_cast<std::uint16_t>(little(2)); }
    std::uint32_t u32() noexcept { return static_cast<std::uint32_t>(little(4)); }
    std::uint64_t u64() noexcept { return little(8); }
    float f32() noexcept { return std::bit_cast<float>(u32()); }
    void skip(std::size_t count) noexcept { pos_ += count; }

private:
    std::uint64_t little(int count) noexcept {
        std::uint64_t value = 0;
        for (int i = 0; i < count; ++i) {
            value |= std::uint64_t{u8()} << (8 * i);
        }
        return value;
    }

    std::span<const std::byte> in_;
    std::size_t pos_ = 0;
};

static_assert(std::numeric_limits<float>::is_iec559, "the wire format sends IEEE 754 floats");

// std::get_if rather than std::visit: visit may throw (a variant left valueless by an
// exception), and these functions promise not to. Every alternative is trivially copyable, so
// a Message is never valueless, but the compiler cannot know that.
MessageType typeOf(const Message& message) noexcept {
    if (std::holds_alternative<JointCommand>(message)) {
        return MessageType::JointCommand;
    }
    if (std::holds_alternative<JointTelemetry>(message)) {
        return MessageType::JointTelemetry;
    }
    return MessageType::Heartbeat;
}

std::size_t payloadSize(MessageType type) noexcept {
    switch (type) {
        case MessageType::Heartbeat:
            return kHeartbeatPayload;
        case MessageType::JointCommand:
            return kCommandPayload;
        case MessageType::JointTelemetry:
            return kTelemetryPayload;
    }
    return 0;
}

void writeJoints(Writer& w, const JointArray& joints) noexcept {
    for (const float value : joints) {
        w.f32(value);
    }
}

void writePayload(Writer& w, const Message& message) noexcept {
    if (const auto* command = std::get_if<JointCommand>(&message)) {
        writeJoints(w, command->target);
    } else if (const auto* telemetry = std::get_if<JointTelemetry>(&message)) {
        writeJoints(w, telemetry->position);
        writeJoints(w, telemetry->velocity);
        w.u8(static_cast<std::uint8_t>(telemetry->state));
        w.u32(telemetry->lastCommandSequence);
    }
}

// A NaN target would make the arm's motion maths NaN; an infinite one has no meaning. Refused
// at the door, so nothing behind it has to wonder.
bool readJoints(Reader& r, JointArray& joints) noexcept {
    bool finite = true;
    for (float& value : joints) {
        value = r.f32();
        finite = finite && std::isfinite(value);
    }
    return finite;
}

bool validState(std::uint8_t state) noexcept {
    return state <= static_cast<std::uint8_t>(ArmState::Holding);
}

std::uint32_t crcOf(std::span<const std::byte> bytes) noexcept {
    return msc::crc32(bytes);
}

}  // namespace

std::size_t encodedSize(const Message& message) noexcept {
    return kHeaderSize + payloadSize(typeOf(message)) + kCrcSize;
}

std::size_t encode(const Packet& packet, std::span<std::byte> out) noexcept {
    const MessageType type = typeOf(packet.message);
    const std::size_t payload = payloadSize(type);
    const std::size_t total = kHeaderSize + payload + kCrcSize;
    if (out.size() < total) {
        return 0;
    }
    Writer w(out);
    w.bytes(kMagic);
    w.u8(kVersion);
    w.u8(static_cast<std::uint8_t>(type));
    w.u16(static_cast<std::uint16_t>(payload));
    w.u32(packet.session);
    w.u32(packet.sequence);
    w.u64(static_cast<std::uint64_t>(packet.timestamp.count()));
    writePayload(w, packet.message);
    w.u32(crcOf(out.first(w.position())));
    return total;
}

DecodeResult decode(std::span<const std::byte> bytes) noexcept {
    if (bytes.size() < kHeaderSize + kCrcSize) {
        return DecodeError::TooShort;
    }
    if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin())) {
        return DecodeError::BadMagic;
    }
    Reader r(bytes);
    r.skip(kMagic.size());
    if (r.u8() != kVersion) {
        return DecodeError::BadVersion;
    }
    const std::uint8_t type = r.u8();
    const std::size_t payload = r.u16();
    if (bytes.size() != kHeaderSize + payload + kCrcSize) {
        return DecodeError::BadLength;
    }
    // Checked before anything in the packet is trusted: a corrupted type or payload must be
    // reported as corruption, not as a strange but intact message.
    Reader crcReader(bytes.subspan(kHeaderSize + payload));
    if (crcReader.u32() != crcOf(bytes.first(kHeaderSize + payload))) {
        return DecodeError::BadCrc;
    }

    Packet packet;
    packet.session = r.u32();
    packet.sequence = r.u32();
    packet.timestamp = Nanoseconds{static_cast<std::int64_t>(r.u64())};

    switch (static_cast<MessageType>(type)) {
        case MessageType::Heartbeat:
            if (payload != kHeartbeatPayload) {
                return DecodeError::BadPayloadSize;
            }
            packet.message = Heartbeat{};
            return packet;
        case MessageType::JointCommand: {
            if (payload != kCommandPayload) {
                return DecodeError::BadPayloadSize;
            }
            JointCommand command;
            if (!readJoints(r, command.target)) {
                return DecodeError::BadValue;
            }
            packet.message = command;
            return packet;
        }
        case MessageType::JointTelemetry: {
            if (payload != kTelemetryPayload) {
                return DecodeError::BadPayloadSize;
            }
            JointTelemetry telemetry;
            const bool positions = readJoints(r, telemetry.position);
            const bool velocities = readJoints(r, telemetry.velocity);
            const std::uint8_t state = r.u8();
            telemetry.lastCommandSequence = r.u32();
            if (!positions || !velocities || !validState(state)) {
                return DecodeError::BadValue;
            }
            telemetry.state = static_cast<ArmState>(state);
            packet.message = telemetry;
            return packet;
        }
    }
    return DecodeError::UnknownType;
}

const char* toString(DecodeError error) noexcept {
    switch (error) {
        case DecodeError::TooShort:
            return "too short";
        case DecodeError::BadMagic:
            return "bad magic";
        case DecodeError::BadVersion:
            return "bad version";
        case DecodeError::BadLength:
            return "bad length";
        case DecodeError::BadCrc:
            return "bad CRC";
        case DecodeError::UnknownType:
            return "unknown type";
        case DecodeError::BadPayloadSize:
            return "bad payload size";
        case DecodeError::BadValue:
            return "bad value";
    }
    return "?";
}

}  // namespace msc::comms
