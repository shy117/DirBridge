#ifndef DIRBRIDGE_TERMINAL_ITERMINALENGINE_H
#define DIRBRIDGE_TERMINAL_ITERMINALENGINE_H

#include "terminal/TerminalTypes.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dirbridge::terminal {

class ITerminalEngine
{
public:
    virtual ~ITerminalEngine() = default;

    virtual bool ingest(const std::uint8_t *bytes, std::size_t size) = 0;
    virtual bool resize(const TerminalGeometry &geometry) = 0;
    virtual TerminalSnapshotPtr snapshot() = 0;
    virtual std::vector<std::uint8_t> encodeKey(
        const TerminalKeyEvent &event) = 0;
    virtual std::vector<std::uint8_t> encodeText(
        const std::string &utf8) = 0;
    virtual std::vector<std::uint8_t> encodePaste(
        const std::string &utf8) = 0;
    virtual std::vector<std::uint8_t> encodeMouse(
        const TerminalMouseEvent &event) = 0;
    virtual bool scrollLines(int lines) = 0;
    virtual std::string lastError() const = 0;
};

} // namespace dirbridge::terminal

#endif // DIRBRIDGE_TERMINAL_ITERMINALENGINE_H
