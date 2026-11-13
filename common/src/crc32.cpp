#include "msc/common/crc32.hpp"

#include <array>

namespace {

constexpr std::array<std::uint32_t, 256> makeTable() noexcept {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < table.size(); ++i) {
        std::uint32_t c = i;
        for (int bit = 0; bit < 8; ++bit) {
            c = (c & 1U) != 0U ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
        }
        table[i] = c;
    }
    return table;
}

// Built at compile time: no start-up cost, no global initialization order issues.
constexpr auto kTable = makeTable();

}  // namespace

std::uint32_t msc::crc32(std::span<const std::byte> data) noexcept {
    std::uint32_t c = 0xFFFFFFFFU;
    for (const std::byte b : data) {
        c = kTable[(c ^ std::to_integer<std::uint32_t>(b)) & 0xFFU] ^ (c >> 8U);
    }
    return c ^ 0xFFFFFFFFU;
}
