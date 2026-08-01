#ifndef DIRBRIDGE_TERMINAL_OPENSSHLAUNCHSPEC_H
#define DIRBRIDGE_TERMINAL_OPENSSHLAUNCHSPEC_H

#include <filesystem>
#include <string>
#include <vector>

namespace dirbridge::terminal {

struct OpenSshLaunchSpec
{
    std::filesystem::path executablePath;
    std::vector<std::wstring> arguments;
    std::filesystem::path workingDirectory;
    bool requiresStoredPasswordAskPass = false;
    std::string redactedSummary;
};

} // namespace dirbridge::terminal

#endif // DIRBRIDGE_TERMINAL_OPENSSHLAUNCHSPEC_H
