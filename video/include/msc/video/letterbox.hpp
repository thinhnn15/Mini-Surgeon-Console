#pragma once

#include <cstdint>

namespace msc::video {

// A rectangle in pixels, origin top-left.
struct PixelRect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;

    friend bool operator==(const PixelRect&, const PixelRect&) = default;
};

// The largest rectangle with the image's aspect ratio that fits inside the target, centred.
// The bars left over (top/bottom or left/right) are not part of the image.
//
// A surgeon must never see a stretched picture: distances and angles in the image are what
// they judge by. So the video is always shown at its own aspect ratio, whatever the window.
// Returns an empty rectangle if either size is zero.
[[nodiscard]] constexpr PixelRect letterbox(std::uint32_t imageWidth, std::uint32_t imageHeight,
                                            std::uint32_t targetWidth, std::uint32_t targetHeight) noexcept {
    if (imageWidth == 0 || imageHeight == 0 || targetWidth == 0 || targetHeight == 0) {
        return {};
    }
    // Compare aspect ratios without division: target is relatively wider than the image
    // when targetW / targetH > imageW / imageH. 64-bit so 8K x 8K cannot overflow.
    const std::uint64_t targetCross = std::uint64_t{targetWidth} * imageHeight;
    const std::uint64_t imageCross = std::uint64_t{imageWidth} * targetHeight;

    PixelRect r;
    if (targetCross > imageCross) {
        // Wider target: full height, bars left and right.
        r.height = targetHeight;
        r.width = static_cast<std::uint32_t>(imageCross / imageHeight);
    } else {
        // Taller (or equal) target: full width, bars top and bottom.
        r.width = targetWidth;
        r.height = static_cast<std::uint32_t>(targetCross / imageWidth);
    }
    r.x = (targetWidth - r.width) / 2;
    r.y = (targetHeight - r.height) / 2;
    return r;
}

}  // namespace msc::video
