#ifndef DIRBRIDGE_TERMINAL_SSHLAUNCHREQUEST_H
#define DIRBRIDGE_TERMINAL_SSHLAUNCHREQUEST_H

#include <cstdint>
#include <optional>
#include <string>

namespace dirbridge::terminal {

enum class SshAuthenticationMode
{
    SystemDefault,
    StoredPassword
};

struct SshLaunchRequest
{
    std::string displayName;
    std::string host;
    std::uint16_t port = 22;
    std::optional<std::string> username;
    SshAuthenticationMode authentication =
        SshAuthenticationMode::SystemDefault;
    bool allowLegacySshRsaHostKey = false;
};

} // namespace dirbridge::terminal

#endif // DIRBRIDGE_TERMINAL_SSHLAUNCHREQUEST_H
