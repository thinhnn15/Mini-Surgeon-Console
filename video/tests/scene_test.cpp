#include "msc/video/scene.hpp"

#include "msc/video/frame.hpp"
#include "msc/video/test_pattern.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace msc::video {
namespace {

using Rgba = std::array<std::uint8_t, 4>;

Rgba pixelAt(const Frame& frame, std::uint32_t x, std::uint32_t y) {
    const std::size_t at = (std::size_t{y} * frame.strideBytes()) + (std::size_t{x} * 4U);
    return {frame.pixels[at], frame.pixels[at + 1], frame.pixels[at + 2], frame.pixels[at + 3]};
}

std::size_t countColour(const Frame& frame, const Rgba& colour) {
    std::size_t count = 0;
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            count += pixelAt(frame, x, y) == colour ? 1U : 0U;
        }
    }
    return count;
}

// A tool coming in from below the bottom-left corner to the middle of a 320x240 frame.
ToolSprite toolTo(double tipX, double tipY) {
    return ToolSprite{.entry = {.x = -60.0, .y = 330.0},
                      .tip = {.x = tipX, .y = tipY},
                      .entryRadius = 12.0,
                      .tipRadius = 4.0};
}

TEST(Scene, TheTipIsFoundWhereItWasDrawn) {
    Frame frame = Frame::allocate(320, 240);
    drawTool(frame, toolTo(200.25, 120.75));

    const std::optional<FramePoint> tip = findToolTip(frame);
    ASSERT_TRUE(tip.has_value());
    // The centre of a disc of whole pixels: within half a pixel of the true centre.
    EXPECT_NEAR(tip->x, 200.25, 0.5);
    EXPECT_NEAR(tip->y, 120.75, 0.5);
}

TEST(Scene, AFrameWithoutAToolHasNoTip) {
    Frame frame = Frame::allocate(320, 240);
    renderTestPattern(frame, 1234);
    EXPECT_FALSE(findToolTip(frame).has_value());
}

// The shaft runs from the tip toward the entry point and looks thicker at the near end.
TEST(Scene, TheShaftGetsThickerTowardTheNearEnd) {
    Frame frame = Frame::allocate(320, 240);
    drawTool(frame, toolTo(200.0, 100.0));

    const auto shaftPixelsInRow = [&frame](std::uint32_t y) {
        std::size_t count = 0;
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            count += pixelAt(frame, x, y) == kToolShaftColour ? 1U : 0U;
        }
        return count;
    };
    const std::size_t nearTip = shaftPixelsInRow(130);
    const std::size_t nearEntry = shaftPixelsInRow(220);
    EXPECT_GT(nearTip, 0U);
    EXPECT_GT(nearEntry, nearTip + 4);
    // Nothing above the tip disc: the shaft ends at the tip.
    EXPECT_EQ(shaftPixelsInRow(80), 0U);
}

// The camera sees the instrument over the pattern, but the barcode strip must stay readable:
// the video checks read the frame number from it.
TEST(Scene, NothingIsDrawnInTheRowsKeptClear) {
    Frame frame = Frame::allocate(320, 240);
    renderTestPattern(frame, 0xA5A5F00FU);
    const std::uint32_t strip = barcodeRows(frame.height);
    ASSERT_GT(strip, 0U);

    // A tool whose tip is right at the top edge: most of the disc would be inside the strip.
    drawTool(frame, toolTo(160.0, 2.0), strip);

    EXPECT_EQ(decodeSequence(frame), std::optional<std::uint32_t>(0xA5A5F00FU));
    for (std::uint32_t y = 0; y < strip; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            const Rgba p = pixelAt(frame, x, y);
            ASSERT_TRUE(p != kToolTipColour && p != kToolShaftColour) << "tool pixel at " << x << ", " << y;
        }
    }
    EXPECT_GT(countColour(frame, kToolShaftColour), 0U) << "the part below the strip is still drawn";
}

// Wherever the tool is, drawing stays inside the frame's buffer. The canary rows after the
// frame would be overwritten by a row or column index that is off by one; under ASan any
// write outside the buffer fails the test by itself.
TEST(Scene, AToolPartlyOrWhollyOutsideTheFrameIsCutOffNotWrittenOutside) {
    const std::vector<ToolSprite> tools{
        toolTo(-500.0, -500.0),  // tip far outside, top left
        toolTo(5000.0, 120.0),   // tip far outside, right
        toolTo(319.9, 239.9),    // tip in the last pixel
        toolTo(0.0, 0.0),        // tip in the first pixel
        ToolSprite{.entry = {.x = 1e12, .y = -1e12},
                   .tip = {.x = -1e12, .y = 1e12},
                   .entryRadius = 1e6,
                   .tipRadius = 1e6},
        ToolSprite{.entry = {.x = 100.0, .y = 100.0},
                   .tip = {.x = 100.0, .y = 100.0},
                   .entryRadius = 5.0,
                   .tipRadius = 5.0},
        ToolSprite{.entry = {.x = 10.0, .y = 50.0},
                   .tip = {.x = 300.0, .y = 50.0},
                   .entryRadius = 3.0,
                   .tipRadius = 3.0},
    };
    for (const ToolSprite& tool : tools) {
        Frame frame = Frame::allocate(320, 240);
        const std::size_t imageBytes = frame.pixels.size();
        frame.pixels.resize(imageBytes + (2 * frame.strideBytes()), 0xEE);  // canary rows
        drawTool(frame, tool);
        EXPECT_TRUE(std::all_of(frame.pixels.begin() + static_cast<std::ptrdiff_t>(imageBytes),
                                frame.pixels.end(), [](std::uint8_t byte) { return byte == 0xEE; }))
            << "tip " << tool.tip.x << ", " << tool.tip.y;
    }
}

// A horizontal shaft (the row-by-row shortcut does not apply to it) is drawn whole.
TEST(Scene, AHorizontalShaftIsDrawnAlongItsWholeLength) {
    Frame frame = Frame::allocate(320, 240);
    drawTool(frame, ToolSprite{.entry = {.x = 10.0, .y = 50.0},
                               .tip = {.x = 300.0, .y = 50.0},
                               .entryRadius = 3.0,
                               .tipRadius = 3.0});
    for (const std::uint32_t x : {12U, 100U, 200U, 290U}) {
        EXPECT_EQ(pixelAt(frame, x, 50), kToolShaftColour) << "x " << x;
    }
    EXPECT_EQ(pixelAt(frame, 150, 60), (Rgba{0, 0, 0, 0})) << "outside the shaft's radius";
}

TEST(Scene, AnEmptyFrameIsLeftAlone) {
    Frame frame;
    drawTool(frame, toolTo(10.0, 10.0));
    EXPECT_TRUE(frame.pixels.empty());
    EXPECT_FALSE(findToolTip(frame).has_value());
}

}  // namespace
}  // namespace msc::video
