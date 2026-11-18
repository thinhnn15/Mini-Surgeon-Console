#include "msc/video/pixel_convert.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace msc::video {
namespace {

struct Rgb {
    int r;
    int g;
    int b;
};

constexpr YuvEncoding kBt601Limited{YuvMatrix::Bt601, YuvRange::Limited};
constexpr YuvEncoding kBt709Limited{YuvMatrix::Bt709, YuvRange::Limited};
constexpr YuvEncoding kBt601Full{YuvMatrix::Bt601, YuvRange::Full};

// Fixed-point rounding and the 8-bit reference values (which are rounded themselves) differ
// by a step or two.
constexpr int kTolerance = 2;

Rgb pixelAt(const Frame& frame, std::uint32_t x, std::uint32_t y) {
    const std::uint8_t* p = frame.pixels.data() + (y * frame.strideBytes()) + (x * 4);
    EXPECT_EQ(p[3], 255) << "alpha at " << x << "," << y;
    return {p[0], p[1], p[2]};
}

void expectNear(const Rgb& actual, const Rgb& expected, const char* what) {
    EXPECT_LE(std::abs(actual.r - expected.r), kTolerance) << what << " red " << actual.r;
    EXPECT_LE(std::abs(actual.g - expected.g), kTolerance) << what << " green " << actual.g;
    EXPECT_LE(std::abs(actual.b - expected.b), kTolerance) << what << " blue " << actual.b;
}

// One pixel pair in YUYV with the same chroma for both.
Rgb convertOne(std::uint8_t y, std::uint8_t cb, std::uint8_t cr, YuvEncoding encoding) {
    const std::array<std::uint8_t, 4> pair{y, cb, y, cr};
    Frame out = Frame::allocate(2, 1);
    EXPECT_TRUE(yuyvToRgba(Plane{pair, pair.size()}, encoding, out));
    return pixelAt(out, 0, 0);
}

TEST(PixelConvert, Bt601LimitedPrimaries) {
    // Reference values from BT.601 (8-bit, limited range): 100 % colour bars.
    expectNear(convertOne(16, 128, 128, kBt601Limited), {0, 0, 0}, "black");
    expectNear(convertOne(235, 128, 128, kBt601Limited), {255, 255, 255}, "white");
    expectNear(convertOne(81, 90, 240, kBt601Limited), {255, 0, 0}, "red");
    expectNear(convertOne(145, 54, 34, kBt601Limited), {0, 255, 0}, "green");
    expectNear(convertOne(41, 240, 110, kBt601Limited), {0, 0, 255}, "blue");
}

TEST(PixelConvert, Bt709LimitedPrimaries) {
    expectNear(convertOne(63, 102, 240, kBt709Limited), {255, 0, 0}, "red");
    expectNear(convertOne(173, 42, 26, kBt709Limited), {0, 255, 0}, "green");
    expectNear(convertOne(32, 240, 118, kBt709Limited), {0, 0, 255}, "blue");
}

TEST(PixelConvert, FullRangeUsesAllOfZeroTo255) {
    expectNear(convertOne(0, 128, 128, kBt601Full), {0, 0, 0}, "black");
    expectNear(convertOne(255, 128, 128, kBt601Full), {255, 255, 255}, "white");
    expectNear(convertOne(128, 128, 128, kBt601Full), {128, 128, 128}, "grey");
}

TEST(PixelConvert, OutOfRangeValuesClampInsteadOfWrapping) {
    // Limited range leaves room above 235 and below 16; cameras do use it. Wrapping around
    // would turn the brightest highlight black.
    const Rgb above = convertOne(255, 128, 128, kBt601Limited);
    EXPECT_EQ(above.r, 255);
    EXPECT_EQ(above.g, 255);
    EXPECT_EQ(above.b, 255);
    const Rgb below = convertOne(0, 128, 128, kBt601Limited);
    EXPECT_EQ(below.r, 0);
    EXPECT_EQ(below.g, 0);
    EXPECT_EQ(below.b, 0);
    // Strongest blue and weakest red chroma on black: blue overshoots 255, red goes negative.
    const Rgb extreme = convertOne(16, 255, 0, kBt601Limited);
    EXPECT_EQ(extreme.b, 255);
    EXPECT_EQ(extreme.r, 0);
}

TEST(PixelConvert, Nv12SharesChromaPerTwoByTwoBlockAndHonoursStrides) {
    // 4x2 image, left block red, right block blue. Both planes padded to 8 bytes per row,
    // with junk in the padding that must not show up.
    constexpr std::size_t kStride = 8;
    const std::vector<std::uint8_t> luma{
        81, 81, 41, 41, 0xEE, 0xEE, 0xEE, 0xEE,  //
        81, 81, 41, 41, 0xEE, 0xEE, 0xEE, 0xEE,
    };
    const std::vector<std::uint8_t> chroma{
        90, 240, 240, 110, 0xEE, 0xEE, 0xEE, 0xEE,  // Cb Cr per block
    };
    Frame out = Frame::allocate(4, 2);
    ASSERT_TRUE(nv12ToRgba(Plane{luma, kStride}, Plane{chroma, kStride}, kBt601Limited, out));
    for (std::uint32_t y = 0; y < 2; ++y) {
        expectNear(pixelAt(out, 0, y), {255, 0, 0}, "left");
        expectNear(pixelAt(out, 1, y), {255, 0, 0}, "left");
        expectNear(pixelAt(out, 2, y), {0, 0, 255}, "right");
        expectNear(pixelAt(out, 3, y), {0, 0, 255}, "right");
    }
}

TEST(PixelConvert, Nv12OddSizeUsesTheLastBlock) {
    // 3x3: the last column and row have no partner, but their chroma block exists.
    const std::vector<std::uint8_t> luma(9, 235);
    const std::vector<std::uint8_t> chroma(8, 128);  // 2x2 blocks of Cb Cr: 4 bytes a row
    Frame out = Frame::allocate(3, 3);
    ASSERT_TRUE(nv12ToRgba(Plane{luma, 3}, Plane{chroma, 4}, kBt601Limited, out));
    expectNear(pixelAt(out, 2, 2), {255, 255, 255}, "corner");
}

TEST(PixelConvert, YuyvAndUyvyOrderTheBytesDifferently) {
    // Two pixels, dark then bright, grey chroma.
    const std::array<std::uint8_t, 4> yuyv{16, 128, 235, 128};
    const std::array<std::uint8_t, 4> uyvy{128, 16, 128, 235};
    Frame a = Frame::allocate(2, 1);
    Frame b = Frame::allocate(2, 1);
    ASSERT_TRUE(yuyvToRgba(Plane{yuyv, 4}, kBt601Limited, a));
    ASSERT_TRUE(uyvyToRgba(Plane{uyvy, 4}, kBt601Limited, b));
    EXPECT_EQ(a.pixels, b.pixels);
    expectNear(pixelAt(a, 0, 0), {0, 0, 0}, "first");
    expectNear(pixelAt(a, 1, 0), {255, 255, 255}, "second");
}

TEST(PixelConvert, BgrxSwapsRedAndBlueAndMakesItOpaque) {
    const std::array<std::uint8_t, 8> bgrx{10, 20, 30, 0, 40, 50, 60, 7};
    Frame out = Frame::allocate(2, 1);
    ASSERT_TRUE(bgrxToRgba(Plane{bgrx, 8}, out));
    EXPECT_EQ(out.pixels, (std::vector<std::uint8_t>{30, 20, 10, 255, 60, 50, 40, 255}));
}

TEST(PixelConvert, RgbxCopiesRowsAndMakesItOpaque) {
    // Stride 12 for a 2-pixel row: 4 bytes of padding to skip.
    const std::vector<std::uint8_t> rgbx{1, 2, 3, 0, 4,  5,  6,  0, 9, 9, 9, 9,  //
                                         7, 8, 9, 0, 10, 11, 12, 0, 9, 9, 9, 9};
    Frame out = Frame::allocate(2, 2);
    ASSERT_TRUE(rgbxToRgba(Plane{rgbx, 12}, out));
    EXPECT_EQ(out.pixels,
              (std::vector<std::uint8_t>{1, 2, 3, 255, 4, 5, 6, 255, 7, 8, 9, 255, 10, 11, 12, 255}));
}

TEST(PixelConvert, BufferTooSmallForTheFrameIsRejectedNotOverread) {
    // A driver reports 4x2 but hands over one byte too few, or a stride shorter than a row.
    // Nothing may be read past the end (ASan would catch it), and nothing is written.
    const std::vector<std::uint8_t> luma(7, 128);
    const std::vector<std::uint8_t> chroma(4, 128);
    Frame out = Frame::allocate(4, 2);
    const std::vector<std::uint8_t> before = out.pixels;
    EXPECT_FALSE(nv12ToRgba(Plane{luma, 4}, Plane{chroma, 4}, kBt601Limited, out));
    EXPECT_FALSE(nv12ToRgba(Plane{luma, 3}, Plane{chroma, 4}, kBt601Limited, out));  // stride < width

    const std::vector<std::uint8_t> packed(15, 128);  // 4x2 YUYV needs 16
    EXPECT_FALSE(yuyvToRgba(Plane{packed, 8}, kBt601Limited, out));
    EXPECT_FALSE(bgrxToRgba(Plane{packed, 16}, out));
    EXPECT_FALSE(rgbxToRgba(Plane{packed, 16}, out));
    EXPECT_EQ(out.pixels, before);
}

TEST(PixelConvert, LastRowNeedsNoPadding) {
    // Many drivers leave the padding off the last row: stride * (rows - 1) + one row is enough.
    const std::vector<std::uint8_t> packed(8 + 4, 128);  // 2x2 YUYV, stride 8, last row 4 bytes
    Frame out = Frame::allocate(2, 2);
    EXPECT_TRUE(yuyvToRgba(Plane{packed, 8}, kBt601Limited, out));
}

}  // namespace
}  // namespace msc::video
