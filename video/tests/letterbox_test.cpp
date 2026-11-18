#include "msc/video/letterbox.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace msc::video {
namespace {

TEST(Letterbox, SameAspectFillsTheTarget) {
    EXPECT_EQ(letterbox(1280, 720, 1920, 1080), (PixelRect{0, 0, 1920, 1080}));
    EXPECT_EQ(letterbox(1280, 720, 640, 360), (PixelRect{0, 0, 640, 360}));
}

TEST(Letterbox, WiderTargetGetsBarsLeftAndRight) {
    // 16:9 video in a very wide area: full height, narrower width, centred.
    EXPECT_EQ(letterbox(1280, 720, 2000, 720), (PixelRect{360, 0, 1280, 720}));
}

TEST(Letterbox, TallerTargetGetsBarsTopAndBottom) {
    // 16:9 video in a 4:3 area: full width.
    EXPECT_EQ(letterbox(1280, 720, 1024, 768), (PixelRect{0, 96, 1024, 576}));
}

TEST(Letterbox, ConstantExpression) {
    static_assert(letterbox(1280, 720, 1280, 720) == PixelRect{0, 0, 1280, 720});
}

TEST(Letterbox, ZeroSizeGivesEmptyRect) {
    EXPECT_EQ(letterbox(0, 720, 100, 100), PixelRect{});
    EXPECT_EQ(letterbox(1280, 720, 100, 0), PixelRect{});
}

TEST(Letterbox, NeverLeavesTheTargetAndKeepsTheAspect) {
    // Odd sizes: rounding must never push the image outside the target, and the aspect
    // error stays below one pixel.
    for (std::uint32_t w = 1; w < 300; w += 7) {
        for (std::uint32_t h = 1; h < 300; h += 11) {
            const PixelRect r = letterbox(1280, 720, w, h);
            ASSERT_LE(r.x + r.width, w);
            ASSERT_LE(r.y + r.height, h);
            ASSERT_TRUE(r.width == w || r.height == h) << w << "x" << h;
            const double ideal = static_cast<double>(r.width) * 720.0 / 1280.0;
            ASSERT_LT(static_cast<double>(r.height) - ideal, 1.0) << w << "x" << h;
            ASSERT_GT(static_cast<double>(r.height) - ideal, -1.0) << w << "x" << h;
        }
    }
}

TEST(Letterbox, HugeSizesDoNotOverflow) {
    const PixelRect r = letterbox(7680, 4320, 100000, 100000);
    EXPECT_EQ(r.width, 100000U);
    EXPECT_EQ(r.height, 56250U);
}

}  // namespace
}  // namespace msc::video
