#include "msc/video/scene.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace msc::video {

namespace {

using Rgba = std::array<std::uint8_t, 4>;

// The rows and columns of the frame a shape may touch, already cut to the frame. Empty if
// first > last on either axis.
struct Box {
    std::int64_t left = 0;
    std::int64_t right = -1;  // inclusive
    std::int64_t top = 0;
    std::int64_t bottom = -1;  // inclusive

    [[nodiscard]] bool empty() const noexcept { return left > right || top > bottom; }
};

Box boxAround(const Frame& frame, double minX, double maxX, double minY, double maxY,
              std::uint32_t keepClearRows) noexcept {
    // Clamp in double first: a point far outside the frame must not overflow the integer.
    const auto width = static_cast<double>(frame.width);
    const auto height = static_cast<double>(frame.height);
    Box box;
    box.left = static_cast<std::int64_t>(std::floor(std::clamp(minX, 0.0, width)));
    box.right = static_cast<std::int64_t>(std::ceil(std::clamp(maxX, -1.0, width - 1.0)));
    const double firstRow = std::min(static_cast<double>(keepClearRows), height);
    box.top = static_cast<std::int64_t>(std::floor(std::clamp(minY, firstRow, height)));
    box.bottom = static_cast<std::int64_t>(std::ceil(std::clamp(maxY, -1.0, height - 1.0)));
    return box;
}

// Writes straight into the pixel buffer. Indexing the vector per pixel is what made the test
// pattern take 130 ms in a Debug build (checked iterators, see the journal for M2.2).
void paintRun(Frame& frame, std::int64_t y, std::int64_t firstX, std::int64_t lastX,
              const Rgba& colour) noexcept {
    if (firstX > lastX) {
        return;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): bounds are the Box's
    std::uint8_t* out = frame.pixels.data() + (static_cast<std::size_t>(y) * frame.strideBytes()) +
                        (static_cast<std::size_t>(firstX) * 4U);
    for (std::int64_t x = firstX; x <= lastX; ++x) {
        std::memcpy(out, colour.data(), colour.size());
        out += colour.size();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    }
}

void drawDisc(Frame& frame, FramePoint centre, double radius, const Rgba& colour,
              std::uint32_t keepClearRows) {
    const Box box = boxAround(frame, centre.x - radius, centre.x + radius, centre.y - radius,
                              centre.y + radius, keepClearRows);
    if (box.empty() || radius <= 0.0) {
        return;
    }
    for (std::int64_t y = box.top; y <= box.bottom; ++y) {
        // The disc's chord on this row (through the pixel centres, at y + 0.5).
        const double dy = (static_cast<double>(y) + 0.5) - centre.y;
        const double half2 = (radius * radius) - (dy * dy);
        if (half2 < 0.0) {
            continue;
        }
        const double half = std::sqrt(half2);
        const auto first = static_cast<std::int64_t>(std::ceil(centre.x - half - 0.5));
        const auto last = static_cast<std::int64_t>(std::floor(centre.x + half - 0.5));
        paintRun(frame, y, std::max(first, box.left), std::min(last, box.right), colour);
    }
}

// A shaft with round ends whose radius goes evenly from `ra` at `a` to `rb` at `b`: every pixel
// whose centre is within the radius of the nearest point of the segment.
void drawShaft(Frame& frame, FramePoint a, FramePoint b, double ra, double rb, const Rgba& colour,
               std::uint32_t keepClearRows) {
    const double maxRadius = std::max(ra, rb);
    const Box box = boxAround(frame, std::min(a.x, b.x) - maxRadius, std::max(a.x, b.x) + maxRadius,
                              std::min(a.y, b.y) - maxRadius, std::max(a.y, b.y) + maxRadius, keepClearRows);
    if (box.empty() || maxRadius <= 0.0) {
        return;
    }
    const double dx = b.x - a.x;
    const double dy = b.y - a.y;
    const double length2 = (dx * dx) + (dy * dy);
    // On one row the shaft covers one run of pixels. Its middle is where the centre line
    // crosses the row; it cannot reach further sideways than this from there. Testing only
    // that stretch instead of the whole box is what keeps a long diagonal shaft cheap.
    const double length = std::sqrt(length2);
    const double steepness = length > 0.0 ? std::abs(dy) / length : 0.0;  // 1 = vertical
    const bool narrowRows = steepness > 0.3;
    const double reach = narrowRows ? (maxRadius / steepness) + 2.0 : 0.0;

    for (std::int64_t y = box.top; y <= box.bottom; ++y) {
        const double py = static_cast<double>(y) + 0.5;
        std::int64_t firstX = box.left;
        std::int64_t lastX = box.right;
        if (narrowRows) {
            const double t = std::clamp((py - a.y) / dy, 0.0, 1.0);
            const double middle = a.x + (t * dx);
            firstX = std::max(firstX, static_cast<std::int64_t>(std::floor(middle - reach)));
            lastX = std::min(lastX, static_cast<std::int64_t>(std::ceil(middle + reach)));
        }
        // Paint the row as one run: find its first and last covered pixel.
        std::int64_t runFirst = -1;
        std::int64_t runLast = -2;
        for (std::int64_t x = firstX; x <= lastX; ++x) {
            const double px = static_cast<double>(x) + 0.5;
            // The nearest point of the segment to this pixel, as a fraction t of the way a -> b.
            const double t =
                length2 > 0.0 ? std::clamp((((px - a.x) * dx) + ((py - a.y) * dy)) / length2, 0.0, 1.0) : 0.0;
            const double ex = px - (a.x + (t * dx));
            const double ey = py - (a.y + (t * dy));
            const double radius = ra + (t * (rb - ra));
            if ((ex * ex) + (ey * ey) <= radius * radius) {
                if (runFirst < 0) {
                    runFirst = x;
                }
                runLast = x;
            }
        }
        if (runFirst >= 0) {
            paintRun(frame, y, runFirst, runLast, colour);
        }
    }
}

}  // namespace

void drawTool(Frame& frame, const ToolSprite& tool, std::uint32_t keepClearRows) {
    if (frame.empty() || frame.format != PixelFormat::Rgba8 ||
        frame.pixels.size() < frame.strideBytes() * frame.height) {
        return;
    }
    drawShaft(frame, tool.entry, tool.tip, tool.entryRadius, tool.tipRadius, kToolShaftColour, keepClearRows);
    drawDisc(frame, tool.tip, tool.tipRadius * kToolTipScale, kToolTipColour, keepClearRows);
}

std::optional<FramePoint> findToolTip(const Frame& frame) {
    if (frame.empty() || frame.pixels.size() < frame.strideBytes() * frame.height) {
        return std::nullopt;
    }
    double sumX = 0.0;
    double sumY = 0.0;
    std::uint64_t count = 0;
    const std::uint8_t* pixel = frame.pixels.data();
    for (std::uint32_t y = 0; y < frame.height; ++y) {
        for (std::uint32_t x = 0; x < frame.width; ++x) {
            if (std::memcmp(pixel, kToolTipColour.data(), kToolTipColour.size()) == 0) {
                sumX += static_cast<double>(x) + 0.5;
                sumY += static_cast<double>(y) + 0.5;
                ++count;
            }
            pixel += kToolTipColour.size();  // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        }
    }
    if (count == 0) {
        return std::nullopt;
    }
    return FramePoint{.x = sumX / static_cast<double>(count), .y = sumY / static_cast<double>(count)};
}

}  // namespace msc::video
