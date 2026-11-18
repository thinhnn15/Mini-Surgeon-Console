#include "msc/video/pixel_convert.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace msc::video {
namespace {

// Coefficients in 16.16 fixed point. Integer maths: exactly the same on every compiler and
// CPU (so tests can expect exact values), and cheaper per pixel than float in a Debug build.
constexpr int kShift = 16;
constexpr int kRound = 1 << (kShift - 1);

struct Coefficients {
    int yOffset;  // 16 for limited range, 0 for full
    int y;        // Y scale
    int rFromCr;
    int gFromCb;  // subtracted
    int gFromCr;  // subtracted
    int bFromCb;
};

int fixed(double value) {
    return static_cast<int>(std::lround(value * (1 << kShift)));
}

// From the standard's definition (Kr and Kb are the red and blue weights in Y):
//   R = Y' + 2 (1 - Kr) Cr
//   B = Y' + 2 (1 - Kb) Cb
//   G = Y' - (2 Kb (1 - Kb) / Kg) Cb - (2 Kr (1 - Kr) / Kg) Cr,  Kg = 1 - Kr - Kb
// with Y' and Cb/Cr stretched from limited range to 0..255 / -128..127 first.
Coefficients coefficients(YuvEncoding encoding) {
    const bool bt709 = encoding.matrix == YuvMatrix::Bt709;
    const double kr = bt709 ? 0.2126 : 0.299;
    const double kb = bt709 ? 0.0722 : 0.114;
    const double kg = 1.0 - kr - kb;
    const bool limited = encoding.range == YuvRange::Limited;
    const double yScale = limited ? 255.0 / 219.0 : 1.0;
    const double cScale = limited ? 255.0 / 224.0 : 1.0;
    return Coefficients{
        .yOffset = limited ? 16 : 0,
        .y = fixed(yScale),
        .rFromCr = fixed(cScale * 2.0 * (1.0 - kr)),
        .gFromCb = fixed(cScale * 2.0 * kb * (1.0 - kb) / kg),
        .gFromCr = fixed(cScale * 2.0 * kr * (1.0 - kr) / kg),
        .bFromCb = fixed(cScale * 2.0 * (1.0 - kb)),
    };
}

// The plane holds `rows` rows of at least `rowBytes` each, `stride` apart. Checked once per
// frame, so the per-pixel loops below can run on plain pointers without reading past the end
// of a buffer whose size or stride a driver got wrong.
bool fits(Plane plane, std::size_t rowBytes, std::uint32_t rows) {
    if (rows == 0) {
        return true;
    }
    return plane.stride >= rowBytes && plane.data.size() >= (plane.stride * (rows - 1)) + rowBytes;
}

bool outputFits(const Frame& out) {
    return out.pixels.size() >= out.strideBytes() * out.height;
}

// Pixel pairs per row in the 4:2:x formats (a lone last pixel still has a whole pair).
std::size_t pairs(std::uint32_t width) {
    return (std::size_t{width} + 1) / 2;
}

std::uint8_t clampToByte(int fixedValue) {
    return static_cast<std::uint8_t>(std::clamp((fixedValue + kRound) >> kShift, 0, 255));
}

// The chroma terms of one block, shared by its 2 (or 4) pixels: computed once per block.
struct Chroma {
    int r;
    int g;
    int b;
};

Chroma chroma(const Coefficients& c, int cb, int cr) {
    cb -= 128;
    cr -= 128;
    return Chroma{.r = c.rFromCr * cr, .g = -(c.gFromCb * cb) - (c.gFromCr * cr), .b = c.bFromCb * cb};
}

// YUYV and UYVY differ only in where Y and Cb/Cr sit inside each 4-byte pair.
struct PackedLayout {
    int y0;
    int cb;
    int y1;
    int cr;
};

// The loops below index rows and pixels with raw pointers: they run once per pixel, 30 times a
// second, and the bounds were checked for the whole buffer before the loop (fits()). std::span
// indexing would add a check per access, which costs a lot in a Debug build (see the journal).
// NOLINTBEGIN(cppcoreguidelines-pro-bounds-pointer-arithmetic)

void writePixel(const Coefficients& c, int luma, const Chroma& ch, std::uint8_t* out) {
    const int y = c.y * (luma - c.yOffset);
    out[0] = clampToByte(y + ch.r);
    out[1] = clampToByte(y + ch.g);
    out[2] = clampToByte(y + ch.b);
    out[3] = 255;
}

std::uint8_t* outputRow(Frame& out, std::uint32_t row) {
    return out.pixels.data() + (std::size_t{row} * out.strideBytes());
}

const std::uint8_t* inputRow(Plane plane, std::uint32_t row) {
    return plane.data.data() + (std::size_t{row} * plane.stride);
}

bool packed422ToRgba(Plane packed, PackedLayout layout, YuvEncoding encoding, Frame& out) {
    if (!outputFits(out) || !fits(packed, pairs(out.width) * 4, out.height)) {
        return false;
    }
    const Coefficients c = coefficients(encoding);
    for (std::uint32_t row = 0; row < out.height; ++row) {
        const std::uint8_t* in = inputRow(packed, row);
        std::uint8_t* dst = outputRow(out, row);
        for (std::uint32_t x = 0; x < out.width; x += 2) {
            const std::uint8_t* pair = in + (std::size_t{x} * 2);
            const Chroma ch = chroma(c, pair[layout.cb], pair[layout.cr]);
            writePixel(c, pair[layout.y0], ch, dst + (std::size_t{x} * 4));
            if (x + 1 < out.width) {
                writePixel(c, pair[layout.y1], ch, dst + ((std::size_t{x} + 1) * 4));
            }
        }
    }
    return true;
}

}  // namespace

bool nv12ToRgba(Plane luma, Plane chromaPlane, YuvEncoding encoding, Frame& out) noexcept {
    if (!outputFits(out) || !fits(luma, out.width, out.height) ||
        !fits(chromaPlane, pairs(out.width) * 2, (out.height + 1) / 2)) {
        return false;
    }
    const Coefficients c = coefficients(encoding);
    for (std::uint32_t row = 0; row < out.height; ++row) {
        const std::uint8_t* y = inputRow(luma, row);
        const std::uint8_t* cbcr = inputRow(chromaPlane, row / 2);
        std::uint8_t* dst = outputRow(out, row);
        for (std::uint32_t x = 0; x < out.width; x += 2) {
            const Chroma ch = chroma(c, cbcr[x], cbcr[x + 1]);
            writePixel(c, y[x], ch, dst + (std::size_t{x} * 4));
            if (x + 1 < out.width) {
                writePixel(c, y[x + 1], ch, dst + ((std::size_t{x} + 1) * 4));
            }
        }
    }
    return true;
}

bool yuyvToRgba(Plane packed, YuvEncoding encoding, Frame& out) noexcept {
    return packed422ToRgba(packed, PackedLayout{.y0 = 0, .cb = 1, .y1 = 2, .cr = 3}, encoding, out);
}

bool uyvyToRgba(Plane packed, YuvEncoding encoding, Frame& out) noexcept {
    return packed422ToRgba(packed, PackedLayout{.y0 = 1, .cb = 0, .y1 = 3, .cr = 2}, encoding, out);
}

bool bgrxToRgba(Plane packed, Frame& out) noexcept {
    if (!outputFits(out) || !fits(packed, out.strideBytes(), out.height)) {
        return false;
    }
    for (std::uint32_t row = 0; row < out.height; ++row) {
        const std::uint8_t* in = inputRow(packed, row);
        std::uint8_t* dst = outputRow(out, row);
        for (std::uint32_t x = 0; x < out.width; ++x) {
            const std::uint8_t* src = in + (std::size_t{x} * 4);
            std::uint8_t* pixel = dst + (std::size_t{x} * 4);
            pixel[0] = src[2];
            pixel[1] = src[1];
            pixel[2] = src[0];
            pixel[3] = 255;
        }
    }
    return true;
}

bool rgbxToRgba(Plane packed, Frame& out) noexcept {
    if (!outputFits(out) || !fits(packed, out.strideBytes(), out.height)) {
        return false;
    }
    for (std::uint32_t row = 0; row < out.height; ++row) {
        const std::uint8_t* in = inputRow(packed, row);
        std::uint8_t* dst = outputRow(out, row);
        std::copy_n(in, out.strideBytes(), dst);
        for (std::uint32_t x = 0; x < out.width; ++x) {
            dst[(std::size_t{x} * 4) + 3] = 255;
        }
    }
    return true;
}

// NOLINTEND(cppcoreguidelines-pro-bounds-pointer-arithmetic)

}  // namespace msc::video
