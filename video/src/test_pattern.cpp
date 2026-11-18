#include "msc/video/test_pattern.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <stdexcept>

namespace msc::video {

namespace {

using Rgba = std::array<std::uint8_t, 4>;

constexpr Rgba kBackground{48, 48, 48, 255};
constexpr Rgba kSquare{0, 200, 80, 255};
constexpr Rgba kBitOne{255, 255, 255, 255};
constexpr Rgba kBitZero{0, 0, 0, 255};

constexpr std::uint32_t kSquareStepPx = 4;
// Decoding thresholds on the red channel: anything in between is "not a clean cell".
constexpr std::uint8_t kBlackMax = 64;
constexpr std::uint8_t kWhiteMin = 192;

struct Layout {
    std::uint32_t barcodeHeight;
    std::uint32_t cellWidth;
    std::uint32_t squareSize;
    std::uint32_t squareTop;
};

Layout layoutFor(std::uint32_t width, std::uint32_t height) noexcept {
    Layout l{};
    l.barcodeHeight = std::max<std::uint32_t>(8, height / 30);
    l.cellWidth = width / kBarcodeBits;
    l.squareSize = std::max<std::uint32_t>(4, height / 8);
    l.squareTop = (height - l.squareSize) / 2;
    return l;
}

// Paints the first row pixel by pixel, then copies that row down: one memmove per row
// instead of one small copy per pixel. In a Debug build (checked iterators) this took a
// 1280x720 frame from ~130 ms to ~1 ms, which matters for a source meant to run at 60 fps.
void fillRect(Frame& frame, std::uint32_t left, std::uint32_t top, std::uint32_t w, std::uint32_t h,
              const Rgba& colour) {
    if (w == 0 || h == 0) {
        return;
    }
    const std::size_t stride = frame.strideBytes();
    const std::size_t rowBytes = std::size_t{w} * 4U;
    const auto rowStart = [&](std::uint32_t y) {
        return frame.pixels.begin() + static_cast<std::ptrdiff_t>((y * stride) + (std::size_t{left} * 4U));
    };

    auto out = rowStart(top);
    for (std::uint32_t x = 0; x < w; ++x) {
        out = std::copy(colour.begin(), colour.end(), out);
    }
    const auto first = rowStart(top);
    for (std::uint32_t y = top + 1; y < top + h; ++y) {
        std::copy_n(first, rowBytes, rowStart(y));
    }
}

}  // namespace

std::uint32_t barcodeRows(std::uint32_t frameHeight) noexcept {
    return layoutFor(kMinPatternWidth, frameHeight).barcodeHeight;
}

std::uint32_t squareLeft(std::uint32_t frameWidth, std::uint32_t frameHeight,
                         std::uint64_t sequence) noexcept {
    const Layout l = layoutFor(frameWidth, frameHeight);
    const std::uint64_t travel = frameWidth - l.squareSize;
    return static_cast<std::uint32_t>((sequence * kSquareStepPx) % travel);
}

void renderTestPattern(Frame& frame, std::uint64_t sequence) {
    if (frame.width < kMinPatternWidth || frame.height < kMinPatternHeight) {
        throw std::invalid_argument("renderTestPattern: frame smaller than the minimum pattern size");
    }
    frame.format = PixelFormat::Rgba8;
    frame.pixels.resize(frame.strideBytes() * frame.height);  // no-op when reused at the same size
    frame.sequence = sequence;

    const Layout l = layoutFor(frame.width, frame.height);

    // The whole image is redrawn: a reused frame still holds a picture from a few frames ago.
    fillRect(frame, 0, 0, frame.width, frame.height, kBackground);

    const auto code = static_cast<std::uint32_t>(sequence);
    for (std::uint32_t bit = 0; bit < kBarcodeBits; ++bit) {
        const bool one = ((code >> (kBarcodeBits - 1 - bit)) & 1U) != 0;
        fillRect(frame, bit * l.cellWidth, 0, l.cellWidth, l.barcodeHeight, one ? kBitOne : kBitZero);
    }

    fillRect(frame, squareLeft(frame.width, frame.height, sequence), l.squareTop, l.squareSize, l.squareSize,
             kSquare);
}

std::optional<std::uint32_t> decodeSequence(const Frame& frame) {
    if (frame.width < kMinPatternWidth || frame.height < kMinPatternHeight ||
        frame.pixels.size() < frame.strideBytes() * frame.height) {
        return std::nullopt;
    }
    const Layout l = layoutFor(frame.width, frame.height);
    const std::size_t sampleRow = l.barcodeHeight / 2;

    std::uint32_t code = 0;
    for (std::uint32_t bit = 0; bit < kBarcodeBits; ++bit) {
        const std::size_t sampleX = (std::size_t{bit} * l.cellWidth) + (l.cellWidth / 2);
        const std::uint8_t red = frame.pixels[(sampleRow * frame.strideBytes()) + (sampleX * 4U)];
        code <<= 1U;
        if (red >= kWhiteMin) {
            code |= 1U;
        } else if (red > kBlackMax) {
            return std::nullopt;
        }
    }
    return code;
}

}  // namespace msc::video
