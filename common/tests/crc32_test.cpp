#include "msc/common/crc32.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string_view>

namespace {

std::span<const std::byte> bytesOf(std::string_view text) {
    return std::as_bytes(std::span(text.data(), text.size()));
}

}  // namespace

// REQ-COM-001: packets carry a CRC-32 so corrupted ones can be discarded.
TEST(Crc32, REQ_COM_001_MatchesStandardCheckValue) {
    // "123456789" -> 0xCBF43926 is the published check value for CRC-32/ISO-HDLC.
    EXPECT_EQ(msc::crc32(bytesOf("123456789")), 0xCBF43926U);
}

TEST(Crc32, EmptyInputIsZero) {
    EXPECT_EQ(msc::crc32({}), 0U);
}

TEST(Crc32, REQ_COM_001_DetectsSingleBitFlip) {
    std::array<std::byte, 4> packet{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    const auto before = msc::crc32(packet);
    packet[2] ^= std::byte{0x10};
    EXPECT_NE(msc::crc32(packet), before);
}
