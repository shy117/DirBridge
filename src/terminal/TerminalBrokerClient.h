#ifndef DIRBRIDGE_TERMINAL_TERMINALBROKERCLIENT_H
#define DIRBRIDGE_TERMINAL_TERMINALBROKERCLIENT_H

#include "terminal/StoredPasswordLease.h"
#include "terminal/TerminalBrokerProtocol.h"
#include "terminal/TerminalBrokerStart.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace dirbridge::terminal::broker {

enum class EventReadResult
{
    Event,
    Eof,
    Error
};

class TerminalBrokerClient
{
public:
    TerminalBrokerClient();
    ~TerminalBrokerClient();
    TerminalBrokerClient(const TerminalBrokerClient &) = delete;
    TerminalBrokerClient &operator=(const TerminalBrokerClient &) = delete;

    bool start(
        const std::filesystem::path &brokerExecutable,
        const StartRequest &request);
    bool start(
        const std::filesystem::path &brokerExecutable,
        const StartRequest &request,
        StoredPasswordLease password);

    bool sendInput(const std::vector<std::uint8_t> &bytes);
    bool resize(std::uint16_t columns, std::uint16_t rows);
    bool close();
    EventReadResult readEvent(Frame &frame);
    bool waitForBroker(
        std::chrono::milliseconds timeout,
        std::uint32_t &exitCode);
    void terminate(std::uint32_t exitCode = 0xE200) noexcept;

    bool started() const noexcept;
    const std::string &error() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dirbridge::terminal::broker

#endif // DIRBRIDGE_TERMINAL_TERMINALBROKERCLIENT_H
