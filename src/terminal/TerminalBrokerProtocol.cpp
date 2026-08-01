#include "terminal/TerminalBrokerProtocol.h"

#include <limits>

namespace dirbridge::terminal::broker {
namespace {

void append16(std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void append32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8)
    {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint16_t read16(const std::uint8_t *bytes)
{
    return static_cast<std::uint16_t>(bytes[0])
        | (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t read32(const std::uint8_t *bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool isKnown(FrameType type) noexcept
{
    return isCommandFrame(type) || isEventFrame(type);
}

} // namespace

bool isCommandFrame(FrameType type) noexcept
{
    switch (type)
    {
    case FrameType::Start:
    case FrameType::AuthSecret:
    case FrameType::Input:
    case FrameType::Resize:
    case FrameType::Close:
        return true;
    default:
        return false;
    }
}

bool isEventFrame(FrameType type) noexcept
{
    switch (type)
    {
    case FrameType::Ready:
    case FrameType::Output:
    case FrameType::Exit:
    case FrameType::Error:
    case FrameType::Stopped:
        return true;
    default:
        return false;
    }
}

std::vector<std::uint8_t> encodeFrame(const Frame &frame)
{
    if (!isKnown(frame.type)
        || frame.payload.size() > MaximumPayloadSize
        || (frame.type == FrameType::AuthSecret
            && frame.payload.size() > MaximumSecretPayloadSize))
    {
        return {};
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve(FrameHeaderSize + frame.payload.size());
    append32(bytes, FrameMagic);
    append16(bytes, ProtocolVersion);
    append16(bytes, FrameHeaderSize);
    append32(bytes, static_cast<std::uint32_t>(frame.type));
    append32(bytes, frame.generation);
    append32(bytes, frame.sequence);
    append32(bytes, static_cast<std::uint32_t>(frame.payload.size()));
    bytes.insert(bytes.end(), frame.payload.begin(), frame.payload.end());
    return bytes;
}

bool decodeFrames(
    const std::vector<std::uint8_t> &bytes,
    std::vector<Frame> &frames,
    std::string &error)
{
    frames.clear();
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        if (bytes.size() - offset < FrameHeaderSize)
        {
            error = "truncated broker frame header";
            return false;
        }
        const std::uint8_t *header = bytes.data() + offset;
        if (read32(header) != FrameMagic
            || read16(header + 4) != ProtocolVersion
            || read16(header + 6) != FrameHeaderSize)
        {
            error = "invalid broker frame preamble";
            return false;
        }

        Frame frame;
        frame.type = static_cast<FrameType>(read32(header + 8));
        frame.generation = read32(header + 12);
        frame.sequence = read32(header + 16);
        const std::uint32_t payloadSize = read32(header + 20);
        if (!isKnown(frame.type))
        {
            error = "unknown broker frame type";
            return false;
        }
        if (payloadSize > MaximumPayloadSize
            || (frame.type == FrameType::AuthSecret
                && payloadSize > MaximumSecretPayloadSize))
        {
            error = "broker frame payload exceeded limit";
            return false;
        }
        if (bytes.size() - offset - FrameHeaderSize < payloadSize)
        {
            error = "truncated broker frame payload";
            return false;
        }
        const auto begin = bytes.begin()
            + static_cast<std::ptrdiff_t>(offset + FrameHeaderSize);
        frame.payload.assign(
            begin,
            begin + static_cast<std::ptrdiff_t>(payloadSize));
        frames.push_back(std::move(frame));
        offset += FrameHeaderSize + payloadSize;
    }
    return true;
}

bool validateCommandSequence(
    const std::vector<Frame> &frames,
    std::string &error)
{
    if (frames.empty() || frames.front().type != FrameType::Start)
    {
        error = "broker command sequence must start with Start";
        return false;
    }
    const std::uint32_t generation = frames.front().generation;
    if (generation == 0 || frames.front().sequence != 1)
    {
        error = "broker start generation or sequence is invalid";
        return false;
    }

    bool sawSecret = false;
    bool sawRuntimeCommand = false;
    bool sawClose = false;
    for (std::size_t index = 0; index < frames.size(); ++index)
    {
        const Frame &frame = frames[index];
        if (!isCommandFrame(frame.type)
            || frame.generation != generation
            || frame.sequence != index + 1)
        {
            error = "broker command generation or sequence is invalid";
            return false;
        }
        if (sawClose)
        {
            error = "broker Close frame must be last";
            return false;
        }
        if (frame.type == FrameType::Start && index != 0)
        {
            error = "broker Start frame was repeated";
            return false;
        }
        if (frame.type == FrameType::AuthSecret)
        {
            if (sawSecret || sawRuntimeCommand || frame.payload.empty())
            {
                error = "broker AuthSecret position or payload is invalid";
                return false;
            }
            sawSecret = true;
        }
        else if (frame.type == FrameType::Input
            || frame.type == FrameType::Resize
            || frame.type == FrameType::Close)
        {
            sawRuntimeCommand = true;
            sawClose = frame.type == FrameType::Close;
        }
    }
    return true;
}

} // namespace dirbridge::terminal::broker
