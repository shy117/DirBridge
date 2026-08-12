#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "terminal/OpenSshLauncher.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <string_view>
#include <vector>

namespace dirbridge::terminal {
namespace {

bool isRegularFile(const std::filesystem::path &path)
{
    std::error_code error;
    return std::filesystem::is_regular_file(path, error) && !error;
}

bool isDirectory(const std::filesystem::path &path)
{
    std::error_code error;
    return std::filesystem::is_directory(path, error) && !error;
}

std::optional<std::wstring> fromUtf8(const std::string &value)
{
    if (value.empty())
    {
        return std::wstring{};
    }

    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (length <= 0)
    {
        return std::nullopt;
    }

    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length)
        != length)
    {
        return std::nullopt;
    }
    return result;
}

bool isValidConnectionToken(
    const std::string &value,
    bool rejectAtSign)
{
    if (value.empty() || value.size() > 255 || value.front() == '-')
    {
        return false;
    }
    for (const unsigned char character : value)
    {
        if (character == '\0'
            || std::iscntrl(character)
            || std::isspace(character)
            || (rejectAtSign && character == '@'))
        {
            return false;
        }
    }
    return true;
}

std::filesystem::path systemOpenSshPath()
{
    std::vector<wchar_t> windowsDirectory(32768);
    const UINT length = GetWindowsDirectoryW(
        windowsDirectory.data(),
        static_cast<UINT>(windowsDirectory.size()));
    if (length == 0 || length >= windowsDirectory.size())
    {
        return {};
    }
    return std::filesystem::path(
        std::wstring(windowsDirectory.data(), length))
        / L"System32"
        / L"OpenSSH"
        / L"ssh.exe";
}

void appendOption(
    std::vector<std::wstring> &arguments,
    std::wstring_view name,
    std::wstring_view value)
{
    arguments.emplace_back(L"-o");
    arguments.emplace_back(std::wstring(name) + L"=" + std::wstring(value));
}

} // namespace

OpenSshLaunchResult OpenSshLauncher::build(
    const SshLaunchRequest &request,
    const OpenSshLauncherConfig &config)
{
    OpenSshLaunchResult result;
    if (!isValidConnectionToken(request.host, true))
    {
        result.error = "SSH host is invalid";
        return result;
    }
    if (request.port == 0)
    {
        result.error = "SSH port must be from 1 to 65535";
        return result;
    }
    if (request.username
        && !isValidConnectionToken(*request.username, false))
    {
        result.error = "SSH username is invalid";
        return result;
    }
    if (!isDirectory(config.workingDirectory)
        || !config.workingDirectory.is_absolute())
    {
        result.error = "SSH working directory must be an existing absolute directory";
        return result;
    }

    std::string locateError;
    const auto executable = locateExecutable(
        config.confirmedExecutablePath,
        locateError);
    if (!executable)
    {
        result.error = std::move(locateError);
        return result;
    }

    const auto host = fromUtf8(request.host);
    const auto username = request.username
        ? fromUtf8(*request.username)
        : std::optional<std::wstring>(std::wstring{});
    if (!host || !username)
    {
        result.error = "SSH host or username is not valid UTF-8";
        return result;
    }

    OpenSshLaunchSpec spec;
    spec.executablePath = *executable;
    spec.workingDirectory = config.workingDirectory;
    spec.arguments.emplace_back(L"-t");
    appendOption(spec.arguments, L"ClearAllForwardings", L"yes");
    appendOption(spec.arguments, L"PermitLocalCommand", L"no");
    appendOption(spec.arguments, L"EnableEscapeCommandline", L"no");
    appendOption(spec.arguments, L"ForwardAgent", L"no");
    appendOption(spec.arguments, L"ForwardX11", L"no");
    appendOption(spec.arguments, L"Tunnel", L"no");
    appendOption(spec.arguments, L"ControlMaster", L"no");
    appendOption(spec.arguments, L"ControlPath", L"none");
    appendOption(spec.arguments, L"ControlPersist", L"no");
    appendOption(spec.arguments, L"ForkAfterAuthentication", L"no");
    appendOption(spec.arguments, L"StdinNull", L"no");
    appendOption(spec.arguments, L"SessionType", L"default");
    appendOption(spec.arguments, L"RemoteCommand", L"none");
    appendOption(spec.arguments, L"AddKeysToAgent", L"no");
    appendOption(spec.arguments, L"StrictHostKeyChecking", L"accept-new");

    if (request.allowLegacySshRsaHostKey)
    {
        appendOption(spec.arguments, L"HostKeyAlgorithms", L"+ssh-rsa");
    }

    if (request.authentication == SshAuthenticationMode::StoredPassword)
    {
        appendOption(spec.arguments, L"PreferredAuthentications", L"password");
        appendOption(spec.arguments, L"PubkeyAuthentication", L"no");
        appendOption(spec.arguments, L"PasswordAuthentication", L"yes");
        appendOption(spec.arguments, L"KbdInteractiveAuthentication", L"no");
        appendOption(spec.arguments, L"NumberOfPasswordPrompts", L"1");
        spec.requiresStoredPasswordAskPass = true;
    }

    spec.arguments.emplace_back(L"-p");
    spec.arguments.emplace_back(std::to_wstring(request.port));
    if (request.username && !request.username->empty())
    {
        spec.arguments.emplace_back(L"-l");
        spec.arguments.emplace_back(*username);
    }
    spec.arguments.emplace_back(*host);
    spec.redactedSummary = spec.requiresStoredPasswordAskPass
        ? "ssh.exe -t [safe-options] [stored-password] -p <redacted> -l <redacted> <redacted>"
        : "ssh.exe -t [safe-options] -p <redacted> -l <redacted> <redacted>";
    result.spec = std::move(spec);
    return result;
}

std::optional<std::filesystem::path> OpenSshLauncher::locateExecutable(
    const std::optional<std::filesystem::path> &confirmedPath,
    std::string &error)
{
    if (confirmedPath)
    {
        if (!confirmedPath->is_absolute() || !isRegularFile(*confirmedPath))
        {
            error = "Confirmed ssh.exe path must be an existing absolute file";
            return std::nullopt;
        }
        return *confirmedPath;
    }

    const std::filesystem::path systemPath = systemOpenSshPath();
    if (!systemPath.empty() && isRegularFile(systemPath))
    {
        return systemPath;
    }

    error = "Windows OpenSSH Client was not found at the system path";
    return std::nullopt;
}

std::wstring OpenSshLauncher::serializeCommandLine(
    const OpenSshLaunchSpec &spec)
{
    std::wstring commandLine = quoteWindowsArgument(
        spec.executablePath.wstring());
    for (const std::wstring &argument : spec.arguments)
    {
        commandLine.push_back(L' ');
        commandLine += quoteWindowsArgument(argument);
    }
    return commandLine;
}

std::wstring OpenSshLauncher::quoteWindowsArgument(
    const std::wstring &value)
{
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : value)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'\"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

} // namespace dirbridge::terminal
