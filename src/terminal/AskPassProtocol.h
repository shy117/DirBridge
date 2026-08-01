#ifndef DIRBRIDGE_TERMINAL_ASKPASSPROTOCOL_H
#define DIRBRIDGE_TERMINAL_ASKPASSPROTOCOL_H

#include <cstddef>
#include <cstdint>

namespace dirbridge::terminal::askpass {

constexpr std::uint32_t Magic = 0x50415344U;
constexpr std::uint16_t Version = 1;
constexpr std::size_t HeaderSize = 12;
constexpr std::size_t MinTokenBytes = 32;
constexpr std::size_t MaxTokenBytes = 128;

enum class ResponseStatus : std::uint16_t
{
    Password = 0,
    Rejected = 1
};

} // namespace dirbridge::terminal::askpass

#endif // DIRBRIDGE_TERMINAL_ASKPASSPROTOCOL_H
