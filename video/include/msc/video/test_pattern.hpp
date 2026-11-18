#pragma once

#include "msc/video/frame.hpp"

#include <cstdint>
#include <optional>

namespace msc::video {

// The synthetic test image. A pure function of (size, sequence number), so it is tested
// without threads or timing, and any frame can be checked against what it should be.
//
//   +--------------------------------------------------+
//   |##__##____####__ ... barcode: sequence, 32 bits   |  top strip
//   |                                                  |
//   |        [] <- square, moves 4 px per frame        |
//   |                                                  |
//   +--------------------------------------------------+
//
// The barcode carries the low 32 bits of the sequence number, most significant bit on the
// left, one cell per bit (white = 1, black = 0). Reading it back from a displayed image
// (M2.4) tells which frame is on screen: that is how latency and dropped frames can be
// checked from the picture itself.

inline constexpr std::uint32_t kBarcodeBits = 32;
// Smallest frame the pattern supports: every barcode cell at least 2 px wide.
inline constexpr std::uint32_t kMinPatternWidth = kBarcodeBits * 2;
inline constexpr std::uint32_t kMinPatternHeight = 32;

// Height of the barcode strip at the top of a frame this high, in rows. Anything drawn over
// the pattern afterwards (the instrument, M5.2) must stay below it.
[[nodiscard]] std::uint32_t barcodeRows(std::uint32_t frameHeight) noexcept;

// Draws the pattern for `sequence` into `frame` and sets frame.sequence. The frame's
// width and height must already be set and be at least the minimum; the pixel buffer is
// resized only if it does not match (so a reused frame is not reallocated).
// Throws std::invalid_argument if the frame is too small.
void renderTestPattern(Frame& frame, std::uint64_t sequence);

// Reads the barcode back. Returns nothing if the frame is too small or any cell is
// neither clearly black nor clearly white (not a test-pattern image, or damaged).
[[nodiscard]] std::optional<std::uint32_t> decodeSequence(const Frame& frame);

// Where renderTestPattern puts the square for this frame: its left edge in pixels.
// Exposed so tests can check motion without reimplementing the drawing.
[[nodiscard]] std::uint32_t squareLeft(std::uint32_t frameWidth, std::uint32_t frameHeight,
                                       std::uint64_t sequence) noexcept;

}  // namespace msc::video
