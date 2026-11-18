#pragma once

#include "msc/video/frame.hpp"

#include <array>
#include <cstdint>
#include <optional>

namespace msc::video {

// A point in a frame, in pixels: (0, 0) is the top-left corner of the top-left pixel. It may
// lie outside the frame.
struct FramePoint {
    double x = 0.0;
    double y = 0.0;
};

// The instrument as the simulated camera sees it (M5.2): a shaft from `entry` to `tip`,
// thicker at the near end, with a coloured tip.
struct ToolSprite {
    FramePoint entry{};
    FramePoint tip{};
    double entryRadius = 0.0;  // pixels
    double tipRadius = 0.0;    // pixels
};

// The colours the instrument is drawn in. The tip's colour appears nowhere else in the test
// pattern, so the tip can be found again in a picture of the screen (M5.4).
inline constexpr std::array<std::uint8_t, 4> kToolShaftColour{176, 184, 196, 255};
inline constexpr std::array<std::uint8_t, 4> kToolTipColour{255, 136, 0, 255};

// The tip is drawn as a disc this many times the shaft's radius at the tip.
inline constexpr double kToolTipScale = 1.6;

// Draws the instrument into an RGBA frame, over whatever is there. Parts outside the frame are
// cut off, and nothing is drawn in the top `keepClearRows` rows (the test pattern's barcode
// must stay readable). Does not allocate.
void drawTool(Frame& frame, const ToolSprite& tool, std::uint32_t keepClearRows = 0);

// The centre of the pixels that have the tip's colour, or nothing if there are none. For
// tests: finds where drawTool put the tip without knowing how it drew it.
[[nodiscard]] std::optional<FramePoint> findToolTip(const Frame& frame);

}  // namespace msc::video
