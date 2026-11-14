#include "msc/video/test_pattern.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <stdexcept>

using msc::video::decodeSequence;
using msc::video::Frame;
using msc::video::renderTestPattern;
using msc::video::squareLeft;

namespace {

constexpr std::uint32_t kWidth = 320;
constexpr std::uint32_t kHeight = 240;

std::array<std::uint8_t, 4> pixelAt(const Frame& f, std::uint32_t x, std::uint32_t y) {
    const std::size_t i = (y * f.strideBytes()) + (std::size_t{x} * 4U);
    return {f.pixels[i], f.pixels[i + 1], f.pixels[i + 2], f.pixels[i + 3]};
}

}  // namespace

TEST(TestPattern, BarcodeRoundTrips) {
    Frame frame = Frame::allocate(kWidth, kHeight);
    for (const std::uint64_t seq : {0ULL, 1ULL, 2ULL, 0x5A5A5A5AULL, 123'456ULL, 0xFFFF'FFFFULL}) {
        renderTestPattern(frame, seq);
        EXPECT_EQ(frame.sequence, seq);
        EXPECT_EQ(decodeSequence(frame), static_cast<std::uint32_t>(seq)) << "sequence " << seq;
    }
}

TEST(TestPattern, BarcodeCarriesLow32Bits) {
    Frame frame = Frame::allocate(kWidth, kHeight);
    renderTestPattern(frame, 0x1'0000'0007ULL);
    EXPECT_EQ(decodeSequence(frame), 7U);
}

TEST(TestPattern, WorksAtFullHd720) {
    Frame frame = Frame::allocate(1280, 720);
    renderTestPattern(frame, 42);
    EXPECT_EQ(decodeSequence(frame), 42U);
}

TEST(TestPattern, SquareMovesWithSequence) {
    Frame frame = Frame::allocate(kWidth, kHeight);
    const std::uint32_t midY = kHeight / 2;
    const std::array<std::uint8_t, 4> green{0, 200, 80, 255};

    renderTestPattern(frame, 10);
    const std::uint32_t left10 = squareLeft(kWidth, kHeight, 10);
    EXPECT_EQ(pixelAt(frame, left10 + 1, midY), green);

    renderTestPattern(frame, 11);
    EXPECT_GT(squareLeft(kWidth, kHeight, 11), left10);
    // The old position is background again: the whole frame was redrawn.
    EXPECT_NE(pixelAt(frame, left10, midY), green);
}

TEST(TestPattern, ReusedFrameIsNotReallocated) {
    Frame frame = Frame::allocate(kWidth, kHeight);
    const auto* before = frame.pixels.data();
    renderTestPattern(frame, 1);
    renderTestPattern(frame, 2);
    EXPECT_EQ(frame.pixels.data(), before);
}

TEST(TestPattern, RejectsFramesThatAreTooSmall) {
    Frame frame = Frame::allocate(16, 16);
    EXPECT_THROW(renderTestPattern(frame, 1), std::invalid_argument);
    EXPECT_FALSE(decodeSequence(frame).has_value());
}

TEST(TestPattern, DecodeRejectsAnImageThatIsNotThePattern) {
    Frame frame = Frame::allocate(kWidth, kHeight);
    std::fill(frame.pixels.begin(), frame.pixels.end(), std::uint8_t{128});  // mid-grey everywhere
    EXPECT_FALSE(decodeSequence(frame).has_value());
}
