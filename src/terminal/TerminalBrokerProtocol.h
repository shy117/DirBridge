#ifndef DIRBRIDGE_TERMINAL_TERMINALBROKERPROTOCOL_H
#define DIRBRIDGE_TERMINAL_TERMINALBROKERPROTOCOL_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dirbridge::terminal::broker {

constexpr std::uint32_t FrameMagic = 0x50425444U;
constexpr std::uint16_t ProtocolVersion = 1;
constexpr std::uint16_t FrameHeaderSize = 24;
constexpr std::uint32_t MaximumPayloadSize = 1024U * 1024U;
constexpr std::uint32_t MaximumSecretPayloadSize = 1023U;

enum class FrameType : std::uint32_t
{
    Start = 1,
    AuthSecret = 2,
    Input = 3,
    Resize = 4,
    Close = 5,
    Ready = 101,
    Output = 102,
    Exit = 103,
    Error = 104,
    Stopped = 105
};

struct Frame
{
    FrameType type = FrameType::Error;
    std::uint32_t generation = 0;
    std::uint32_t sequence = 0;
    std::vector<std::uint8_t> payload;
};

bool isCommandFrame(FrameType type) noexcept;
bool isEventFrame(FrameType type) noexcept;

std::vector<std::uint8_t> encodeFrame(const Frame &frame);
bool decodeFrames(
    const std::vector<std::uint8_t> &bytes,
    std::vector<Frame> &frames,
    std::string &error);
bool validateCommandSequence(
    const std::vector<Frame> &frames,
    std::string &error);

} // namespace dirbridge::terminal::broker

#endif // DIRBRIDGE_TERMINAL_TERMINALBROKERPROTOCOL_H
