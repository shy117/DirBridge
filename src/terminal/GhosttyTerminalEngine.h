#ifndef DIRBRIDGE_TERMINAL_GHOSTTYTERMINALENGINE_H
#define DIRBRIDGE_TERMINAL_GHOSTTYTERMINALENGINE_H

#include "terminal/ITerminalEngine.h"

#include <filesystem>
#include <memory>

namespace dirbridge::terminal {

class GhosttyTerminalEngine final : public ITerminalEngine
{
public:
    static std::unique_ptr<GhosttyTerminalEngine> create(
        const std::filesystem::path &libraryPath,
        const TerminalGeometry &geometry,
        std::string &error);

    ~GhosttyTerminalEngine() override;

    bool ingest(const std::uint8_t *bytes, std::size_t size) override;
    bool resize(const TerminalGeometry &geometry) override;
    TerminalSnapshotPtr snapshot() override;
    std::vector<std::uint8_t> encodeKey(
        const TerminalKeyEvent &event) override;
    std::vector<std::uint8_t> encodeText(
        const std::string &utf8) override;
    std::vector<std::uint8_t> encodePaste(
        const std::string &utf8) override;
    std::vector<std::uint8_t> encodeMouse(
        const TerminalMouseEvent &event) override;
    bool scrollLines(int lines) override;
    std::string lastError() const override;

private:
    GhosttyTerminalEngine();
    bool initialize(
        const std::filesystem::path &libraryPath,
        const TerminalGeometry &geometry);
    void setError(std::string message);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dirbridge::terminal

#endif // DIRBRIDGE_TERMINAL_GHOSTTYTERMINALENGINE_H
