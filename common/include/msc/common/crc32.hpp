#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace msc {

// CRC-32 as used by Ethernet and zlib (reflected, polynomial 0xEDB88320).
// Detects corrupted packets; it is not a security check.
[[nodiscard]] std::uint32_t crc32(std::span<const std::byte> data) noexcept;

}  // namespace msc
