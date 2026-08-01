#pragma once

#include <cstddef>
#include <cstdint>

namespace dirbridge::terminal::broker::test {

constexpr std::uint32_t kFrameMagic = 0x50425444U;
constexpr std::uint16_t kProtocolVersion = 1;
constexpr std::uint16_t kFrameHeaderSize = 24;
constexpr std::uint32_t kMaximumPayloadSize = 1024U * 1024U;

enum class FrameType : std::uint32_t
{
    Start = 1,
    Ready = 2,
    Stopped = 3,
    Error = 4,
};

struct Frame
{
    FrameType type = FrameType::Error;
    std::uint32_t generation = 0;
    std::uint32_t sequence = 0;
};

} // namespace dirbridge::terminal::broker::test
