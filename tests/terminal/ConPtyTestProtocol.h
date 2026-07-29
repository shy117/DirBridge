#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace dirbridge::terminal::test {

inline constexpr std::size_t kOutputBlockSize = 4096;
inline constexpr std::size_t kOutputBlockCount = 16 * 1024;
inline constexpr std::size_t kOutputLineSize = 128;
inline constexpr std::size_t kOutputByteCount =
    kOutputBlockSize * kOutputBlockCount;
inline constexpr std::string_view kOutputTailMarker =
    "DIRBRIDGE_CONPTY_64M_END";

inline std::array<std::uint8_t, kOutputBlockSize> makeOutputBlock(
    std::size_t sequence)
{
    std::array<std::uint8_t, kOutputBlockSize> block{};
    for (std::size_t index = 0; index < block.size(); ++index) {
        block[index] = static_cast<std::uint8_t>(
            'A' + ((sequence + index) % 26));
    }

    static constexpr char kHexDigits[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < 16; ++index) {
        const auto shift = static_cast<unsigned>((15 - index) * 4);
        block[index] = static_cast<std::uint8_t>(
            kHexDigits[(sequence >> shift) & 0x0f]);
    }
    block[16] = ':';
    for (std::size_t lineOffset = 0;
         lineOffset < block.size();
         lineOffset += kOutputLineSize) {
        block[lineOffset + kOutputLineSize - 2] = '\r';
        block[lineOffset + kOutputLineSize - 1] = '\n';
    }

    if (sequence + 1 == kOutputBlockCount) {
        const auto markerOffset =
            block.size() - 2 - kOutputTailMarker.size();
        for (std::size_t index = 0; index < kOutputTailMarker.size(); ++index) {
            block[markerOffset + index] =
                static_cast<std::uint8_t>(kOutputTailMarker[index]);
        }
    }

    return block;
}

} // namespace dirbridge::terminal::test
