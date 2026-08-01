#ifndef DIRBRIDGE_TERMINAL_OPENSSHLAUNCHER_H
#define DIRBRIDGE_TERMINAL_OPENSSHLAUNCHER_H

#include <filesystem>
#include <optional>
#include <string>

#include "terminal/OpenSshLaunchSpec.h"
#include "terminal/SshLaunchRequest.h"

namespace dirbridge::terminal {

struct OpenSshLauncherConfig
{
    std::optional<std::filesystem::path> confirmedExecutablePath;
    std::filesystem::path workingDirectory;
};

struct OpenSshLaunchResult
{
    std::optional<OpenSshLaunchSpec> spec;
    std::string error;
};

class OpenSshLauncher
{
public:
    static OpenSshLaunchResult build(
        const SshLaunchRequest &request,
        const OpenSshLauncherConfig &config);

    static std::optional<std::filesystem::path> locateExecutable(
        const std::optional<std::filesystem::path> &confirmedPath,
        std::string &error);

    static std::wstring serializeCommandLine(
        const OpenSshLaunchSpec &spec);

private:
    static std::wstring quoteWindowsArgument(const std::wstring &value);
};

} // namespace dirbridge::terminal

#endif // DIRBRIDGE_TERMINAL_OPENSSHLAUNCHER_H
