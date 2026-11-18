#pragma once

#include "msc/common/clock.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace msc::video {

enum class PixelFormat : std::uint8_t {
    Rgba8,  // 4 bytes per pixel: R, G, B, A. A camera may later add NV12 / YUYV (M2.5).
};

// One video frame. Plain data, no Qt, so it can be produced and tested anywhere.
//
// Frames are not passed around by value on the video path: three of them live inside a
// TripleBuffer and are reused (ADR 0002). That is why there is an allocate() step and
// why writers overwrite every field.
struct Frame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::Rgba8;
    // Counts up from 1 per frame produced by a source; 0 means "no frame yet".
    std::uint64_t sequence = 0;
    // When the image was captured, on the console's monotonic clock. Latency (M2.4) is
    // measured from here.
    Nanoseconds captureTime{0};
    // Row-major, top row first, no padding between rows: size == strideBytes() * height.
    std::vector<std::uint8_t> pixels;

    [[nodiscard]] std::size_t strideBytes() const noexcept { return std::size_t{width} * 4U; }
    [[nodiscard]] bool empty() const noexcept { return width == 0 || height == 0; }

    // A frame with its pixel buffer allocated (and zeroed) for the given size.
    [[nodiscard]] static Frame allocate(std::uint32_t width, std::uint32_t height) {
        Frame frame;
        frame.width = width;
        frame.height = height;
        frame.pixels.resize(frame.strideBytes() * height);
        return frame;
    }
};

}  // namespace msc::video
