#ifndef DIRBRIDGE_TERMINAL_CONPTYPROCESS_H
#define DIRBRIDGE_TERMINAL_CONPTYPROCESS_H

#include "terminal/OpenSshLaunchSpec.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dirbridge::terminal {

class ConPtyProcess
{
public:
    ConPtyProcess();
    ConPtyProcess(const ConPtyProcess &) = delete;
    ConPtyProcess &operator=(const ConPtyProcess &) = delete;
    ~ConPtyProcess();

    bool start(
        const OpenSshLaunchSpec &spec,
        const std::vector<std::pair<std::wstring, std::wstring>>
            &environmentOverrides,
        std::uint16_t columns,
        std::uint16_t rows);
    bool send(const std::vector<std::uint8_t> &bytes);
    bool resize(std::uint16_t columns, std::uint16_t rows);
    void closeInput() noexcept;
    bool hasExited() const noexcept;
    bool wait(std::chrono::milliseconds timeout, std::uint32_t &exitCode);
    void terminate(std::uint32_t exitCode) noexcept;

    std::vector<std::uint8_t> takeOutput();
    const std::string &error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dirbridge::terminal

#endif // DIRBRIDGE_TERMINAL_CONPTYPROCESS_H
