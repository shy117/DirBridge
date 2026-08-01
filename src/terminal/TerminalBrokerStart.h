#ifndef DIRBRIDGE_TERMINAL_TERMINALBROKERSTART_H
#define DIRBRIDGE_TERMINAL_TERMINALBROKERSTART_H

#include "terminal/SshLaunchRequest.h"

#include <filesystem>
#include <string>
#include <vector>

namespace dirbridge::terminal::broker {

struct StartRequest
{
    SshLaunchRequest ssh;
    std::filesystem::path sshExecutable;
    std::filesystem::path workingDirectory;
    std::filesystem::path askPassHelper;
};

std::vector<std::uint8_t> encodeStartRequest(const StartRequest &request);
bool decodeStartRequest(
    const std::vector<std::uint8_t> &payload,
    StartRequest &request,
    std::string &error);

} // namespace dirbridge::terminal::broker

#endif // DIRBRIDGE_TERMINAL_TERMINALBROKERSTART_H
