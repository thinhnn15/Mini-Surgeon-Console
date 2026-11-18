#pragma once

#include "msc/video/frame.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

namespace msc::video {

// Colour conversion from what cameras deliver to the RGBA the renderer uploads. On the CPU,
// in integer fixed point; no Qt, so it is tested here with known colours.
//
// Cameras rarely give RGB. They send Y'CbCr ("YUV"): brightness at full resolution and the
// two colour-difference channels at half resolution, because the eye sees detail in
// brightness far better than in colour. Two layouts cover nearly every webcam:
//   NV12  4:2:0, two planes: Y (one byte per pixel), then Cb Cr interleaved, one pair per
//         2x2 block of pixels. 12 bits per pixel.
//   YUYV  4:2:2, one plane: Y0 Cb Y1 Cr for each pair of pixels (UYVY: Cb Y0 Cr Y1).
//         16 bits per pixel.
//
// Converting on the CPU costs one pass over every pixel per frame (measured in the journal).
// Doing it in the fragment shader instead (upload the planes as they are, convert while
// sampling) would remove that pass and halve the upload: a later step.

// Which RGB primaries the camera's numbers refer to. Standard definition (BT.601) and HD
// (BT.709) weigh red, green and blue differently when they build Y.
enum class YuvMatrix : std::uint8_t {
    Bt601,
    Bt709,
};

// Limited ("video") range: Y uses 16..235 and Cb/Cr 16..240, leaving head- and footroom.
// Full range uses 0..255 for all three. Most webcams send limited range.
enum class YuvRange : std::uint8_t {
    Limited,
    Full,
};

struct YuvEncoding {
    YuvMatrix matrix = YuvMatrix::Bt601;
    YuvRange range = YuvRange::Limited;
};

// One image plane as the camera's buffer has it: all of its bytes, and the stride (bytes from
// one row to the next, often more than the visible width: rows are padded for alignment).
struct Plane {
    std::span<const std::uint8_t> data;
    std::size_t stride = 0;
};

// Each function fills out.pixels for out.width x out.height (the caller allocates the frame
// at the camera's size) and touches nothing else in the frame. Alpha is always 255.
// Odd widths and heights are fine: the last column / row uses the chroma of its block.
//
// Each returns false, and writes nothing, if a plane is too small for the frame size and
// stride (or the stride is shorter than a row): a driver's mistake must not make the console
// read past the end of a buffer. The check is once per frame, not per pixel.
[[nodiscard]] bool nv12ToRgba(Plane luma, Plane chroma, YuvEncoding encoding, Frame& out) noexcept;
[[nodiscard]] bool yuyvToRgba(Plane packed, YuvEncoding encoding, Frame& out) noexcept;
[[nodiscard]] bool uyvyToRgba(Plane packed, YuvEncoding encoding, Frame& out) noexcept;
// 8-bit B G R X / B G R A (the usual Windows layout): swaps red and blue, forces alpha.
[[nodiscard]] bool bgrxToRgba(Plane packed, Frame& out) noexcept;
// 8-bit R G B X / R G B A: copies, forces alpha (X is undefined, A is not wanted: the video
// is opaque).
[[nodiscard]] bool rgbxToRgba(Plane packed, Frame& out) noexcept;

}  // namespace msc::video
