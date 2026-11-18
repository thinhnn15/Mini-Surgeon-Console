#include "msc/comms/protocol.hpp"

#include "msc/common/crc32.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <variant>
#include <vector>

namespace msc::comms {
namespace {

using Bytes = std::vector<std::byte>;

Packet heartbeat() {
    return Packet{.session = 0x01020304,
                  .sequence = 5,
                  .timestamp = Nanoseconds{0x1122334455667788},
                  .message = Heartbeat{}};
}

Packet command() {
    return Packet{.session = 0xCAFEF00D,
                  .sequence = 42,
                  .timestamp = Nanoseconds{123'456'789},
                  .message = JointCommand{.target = {0.1F, -0.2F, 0.3F, -1.5F, 2.0F, 0.0F}}};
}

Packet telemetry() {
    return Packet{.session = 7,
                  .sequence = 0xFFFFFFFF,
                  .timestamp = Nanoseconds{-1},  // the sender's clock is its own business
                  .message = JointTelemetry{.position = {1, 2, 3, 4, 5, 6},
                                            .velocity = {-0.5F, 0, 0, 0, 0, 0.25F},
                                            .state = ArmState::Holding,
                                            .lastCommandSequence = 41}};
}

Bytes encoded(const Packet& packet) {
    Bytes bytes(kMaxPacketSize);
    bytes.resize(encode(packet, bytes));
    return bytes;
}

bool decodes(const Bytes& bytes) {
    return std::holds_alternative<Packet>(decode(bytes));
}

DecodeError errorOf(const Bytes& bytes) {
    const DecodeResult result = decode(bytes);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(result));
    return std::holds_alternative<DecodeError>(result) ? std::get<DecodeError>(result)
                                                       : DecodeError::TooShort;
}

// For tests that break a rule on purpose behind an intact CRC: recompute it after editing.
void fixCrc(Bytes& bytes) {
    const std::size_t body = bytes.size() - kCrcSize;
    const std::uint32_t crc = msc::crc32(std::span(bytes).first(body));
    for (std::size_t i = 0; i < kCrcSize; ++i) {
        bytes[body + i] = static_cast<std::byte>(crc >> (8 * i));
    }
}

Packet hold() {
    return Packet{.session = 3, .sequence = 77, .timestamp = Nanoseconds{9}, .message = HoldCommand{}};
}

TEST(Protocol, EveryMessageSurvivesARoundTrip) {
    for (const Packet& packet : {heartbeat(), command(), telemetry(), hold()}) {
        const Bytes bytes = encoded(packet);
        ASSERT_EQ(bytes.size(), encodedSize(packet.message));
        const DecodeResult result = decode(bytes);
        ASSERT_TRUE(std::holds_alternative<Packet>(result)) << toString(std::get<DecodeError>(result));
        EXPECT_EQ(std::get<Packet>(result), packet);
    }
}

TEST(Protocol, SizesOfVersion1) {
    EXPECT_EQ(encodedSize(Heartbeat{}), 28U);
    EXPECT_EQ(encodedSize(JointCommand{}), 52U);
    EXPECT_EQ(encodedSize(JointTelemetry{}), 81U);
    EXPECT_EQ(encodedSize(HoldCommand{}), 28U);
    EXPECT_LE(encodedSize(JointTelemetry{}), kMaxPacketSize);
}

TEST(Protocol, HeaderIsLittleEndianByteForByte) {
    // Pinned bytes, so a change of layout or byte order cannot slip through a round trip
    // (which would pass with any consistent pair of bugs).
    const Bytes bytes = encoded(heartbeat());
    const std::array<std::uint8_t, kHeaderSize> expected{
        'M',  'S',  'C',  'L',                           // magic
        1,                                               // version
        1,                                               // type: heartbeat
        0,    0,                                         // payload length
        0x04, 0x03, 0x02, 0x01,                          // session 0x01020304
        5,    0,    0,    0,                             // sequence 5
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,  // timestamp 0x1122334455667788
    };
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(std::to_integer<std::uint8_t>(bytes[i]), expected[i]) << "byte " << i;
    }
}

TEST(Protocol, REQ_COM_001_EverySingleBitFlipIsRefused) {
    for (const Packet& packet : {heartbeat(), command(), telemetry(), hold()}) {
        const Bytes original = encoded(packet);
        for (std::size_t bit = 0; bit < original.size() * 8; ++bit) {
            Bytes corrupted = original;
            corrupted[bit / 8] ^= static_cast<std::byte>(1U << (bit % 8));
            EXPECT_FALSE(decodes(corrupted))
                << "bit " << bit << " of a " << original.size() << "-byte packet";
        }
    }
}

TEST(Protocol, REQ_COM_001_TruncatedOrExtendedPacketIsRefused) {
    const Bytes original = encoded(telemetry());
    for (std::size_t length = 0; length < original.size(); ++length) {
        EXPECT_FALSE(decodes(Bytes(original.begin(), original.begin() + static_cast<std::ptrdiff_t>(length))))
            << "length " << length;
    }
    Bytes longer = original;
    longer.push_back(std::byte{0});
    EXPECT_EQ(errorOf(longer), DecodeError::BadLength);
}

TEST(Protocol, REQ_COM_001_RandomBytesNeverDecodeAndNeverCrash) {
    // Under ASan/UBSan in CI this also proves decode() never reads outside the buffer.
    std::mt19937 random(2026);
    std::uniform_int_distribution<int> byte(0, 255);
    std::uniform_int_distribution<std::size_t> length(0, kMaxPacketSize + 16);
    for (int i = 0; i < 100'000; ++i) {
        Bytes bytes(length(random));
        for (std::byte& b : bytes) {
            b = static_cast<std::byte>(byte(random));
        }
        EXPECT_FALSE(decodes(bytes));
    }
    // The same with a valid header in front: now the length and CRC checks do the work.
    const Bytes valid = encoded(command());
    for (int i = 0; i < 100'000; ++i) {
        Bytes bytes = valid;
        for (std::size_t j = kHeaderSize; j < bytes.size(); ++j) {
            bytes[j] = static_cast<std::byte>(byte(random));
        }
        EXPECT_FALSE(decodes(bytes));
    }
}

TEST(Protocol, ReasonsForRefusal) {
    const Bytes valid = encoded(command());

    Bytes magic = valid;
    magic[0] = std::byte{'X'};
    EXPECT_EQ(errorOf(magic), DecodeError::BadMagic);

    Bytes version = valid;
    version[4] = std::byte{2};
    fixCrc(version);
    EXPECT_EQ(errorOf(version), DecodeError::BadVersion);

    Bytes crc = valid;
    crc[kHeaderSize] ^= std::byte{0x01};
    EXPECT_EQ(errorOf(crc), DecodeError::BadCrc);

    Bytes type = valid;
    type[5] = std::byte{99};
    fixCrc(type);
    EXPECT_EQ(errorOf(type), DecodeError::UnknownType);

    // A heartbeat with a command's payload: intact, but the wrong size for its type.
    Bytes size = valid;
    size[5] = std::byte{static_cast<std::uint8_t>(MessageType::Heartbeat)};
    fixCrc(size);
    EXPECT_EQ(errorOf(size), DecodeError::BadPayloadSize);

    EXPECT_EQ(errorOf(Bytes(10)), DecodeError::TooShort);
}

TEST(Protocol, REQ_COM_001_NonFiniteJointValuesAreRefused) {
    // A NaN target would turn the arm's motion maths into NaN. It is refused even with a
    // perfect CRC: the sender is wrong, not the wire.
    for (const float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
                            -std::numeric_limits<float>::infinity()}) {
        Packet packet = command();
        std::get<JointCommand>(packet.message).target[3] = bad;
        EXPECT_EQ(errorOf(encoded(packet)), DecodeError::BadValue);

        Packet report = telemetry();
        std::get<JointTelemetry>(report.message).velocity[0] = bad;
        EXPECT_EQ(errorOf(encoded(report)), DecodeError::BadValue);
    }
}

TEST(Protocol, UnknownArmStateIsRefused) {
    Bytes bytes = encoded(telemetry());
    bytes[kHeaderSize + (2 * kJointCount * 4)] = std::byte{7};  // the state byte
    fixCrc(bytes);
    EXPECT_EQ(errorOf(bytes), DecodeError::BadValue);
}

TEST(Protocol, EncodeIntoATooSmallBufferWritesNothing) {
    Bytes small(encodedSize(JointCommand{}) - 1, std::byte{0xAB});
    EXPECT_EQ(encode(command(), small), 0U);
    for (const std::byte b : small) {
        EXPECT_EQ(b, std::byte{0xAB});
    }
}

}  // namespace
}  // namespace msc::comms
