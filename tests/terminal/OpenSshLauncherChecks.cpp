#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "terminal/OpenSshLauncher.h"
#include "terminal/StoredPasswordLease.h"

namespace {

using dirbridge::terminal::OpenSshLaunchSpec;
using dirbridge::terminal::OpenSshLauncher;
using dirbridge::terminal::OpenSshLauncherConfig;
using dirbridge::terminal::SshAuthenticationMode;
using dirbridge::terminal::SshLaunchRequest;

class Reporter
{
public:
    void check(bool condition, const char *label)
    {
        std::cout << (condition ? "[PASS] " : "[FAIL] ")
                  << label << std::endl;
        if (!condition)
        {
            ++failures_;
        }
    }

    int failures() const
    {
        return failures_;
    }

private:
    int failures_ = 0;
};

bool hasOption(
    const OpenSshLaunchSpec &spec,
    const std::wstring &value)
{
    for (const std::wstring &argument : spec.arguments)
    {
        if (argument == value)
        {
            return true;
        }
    }
    return false;
}

std::vector<std::wstring> parseCommandLine(
    const std::wstring &commandLine)
{
    int count = 0;
    wchar_t **arguments = CommandLineToArgvW(
        commandLine.c_str(),
        &count);
    if (!arguments)
    {
        return {};
    }
    std::vector<std::wstring> result;
    for (int index = 0; index < count; ++index)
    {
        result.emplace_back(arguments[index]);
    }
    LocalFree(arguments);
    return result;
}

} // namespace

int main()
{
    Reporter reporter;
    std::string locateError;
    const auto sshPath = OpenSshLauncher::locateExecutable(
        std::nullopt,
        locateError);
    reporter.check(
        sshPath.has_value(),
        "locate system OpenSSH executable");
    if (!sshPath)
    {
        std::cout << "[DETAIL] " << locateError << std::endl;
        return 1;
    }

    OpenSshLauncherConfig config;
    config.confirmedExecutablePath = *sshPath;
    config.workingDirectory = std::filesystem::temp_directory_path();

    SshLaunchRequest request;
    request.displayName = "Ubuntu";
    request.host = "127.0.0.1";
    request.port = 22;
    request.username = "test-user";
    const auto defaultResult = OpenSshLauncher::build(request, config);
    reporter.check(
        defaultResult.spec.has_value(),
        "build system-default SSH launch spec");
    if (!defaultResult.spec)
    {
        std::cout << "[DETAIL] " << defaultResult.error << std::endl;
        return 1;
    }
    reporter.check(
        !defaultResult.spec->requiresStoredPasswordAskPass,
        "system-default spec does not request stored password");
    reporter.check(
        !hasOption(
            *defaultResult.spec,
            L"PreferredAuthentications=password"),
        "system-default spec preserves OpenSSH authentication policy");
    reporter.check(
        hasOption(*defaultResult.spec, L"ClearAllForwardings=yes")
            && hasOption(*defaultResult.spec, L"PermitLocalCommand=no")
            && hasOption(*defaultResult.spec, L"ControlMaster=no"),
        "fixed safety options are present");

    request.authentication = SshAuthenticationMode::StoredPassword;
    const auto passwordResult = OpenSshLauncher::build(request, config);
    reporter.check(
        passwordResult.spec.has_value(),
        "build stored-password SSH launch spec");
    if (!passwordResult.spec)
    {
        std::cout << "[DETAIL] " << passwordResult.error << std::endl;
        return 1;
    }
    reporter.check(
        passwordResult.spec->requiresStoredPasswordAskPass,
        "stored-password spec requires AskPass transport");
    reporter.check(
        hasOption(
            *passwordResult.spec,
            L"PreferredAuthentications=password")
            && hasOption(
                *passwordResult.spec,
                L"PubkeyAuthentication=no")
            && hasOption(
                *passwordResult.spec,
                L"KbdInteractiveAuthentication=no")
            && hasOption(
                *passwordResult.spec,
                L"NumberOfPasswordPrompts=1"),
        "stored-password authentication is deterministic");

    const std::wstring commandLine =
        OpenSshLauncher::serializeCommandLine(*passwordResult.spec);
    const std::vector<std::wstring> parsed = parseCommandLine(commandLine);
    reporter.check(
        parsed.size() == passwordResult.spec->arguments.size() + 1
            && parsed.front()
                == passwordResult.spec->executablePath.wstring()
            && std::equal(
                passwordResult.spec->arguments.begin(),
                passwordResult.spec->arguments.end(),
                parsed.begin() + 1),
        "serialized command line round-trips through CommandLineToArgvW");

    request.host = "-bad-host";
    reporter.check(
        !OpenSshLauncher::build(request, config).spec,
        "reject option-like host");
    request.host = "bad host";
    reporter.check(
        !OpenSshLauncher::build(request, config).spec,
        "reject host containing whitespace");
    request.host = "user@example.com";
    reporter.check(
        !OpenSshLauncher::build(request, config).spec,
        "reject host containing username separator");
    request.host = "127.0.0.1";
    request.port = 0;
    reporter.check(
        !OpenSshLauncher::build(request, config).spec,
        "reject zero SSH port");

    {
        dirbridge::terminal::StoredPasswordLease password("site-secret");
        reporter.check(
            password.size() == 11 && !password.consumed(),
            "stored password lease starts with expected state");
        bool callbackRan = false;
        const bool consumed = password.consume(
            [&callbackRan](const char *bytes, std::size_t size) {
                callbackRan = true;
                return std::string(bytes, size) == "site-secret";
            });
        reporter.check(
            consumed && callbackRan && password.empty()
                && password.consumed(),
            "stored password lease consumes and clears once");
        reporter.check(
            !password.consume([](const char *, std::size_t) { return true; }),
            "stored password lease rejects a second consume");
    }

    bool rejectedLineBreak = false;
    try
    {
        dirbridge::terminal::StoredPasswordLease password("bad\npassword");
    }
    catch (const std::invalid_argument &)
    {
        rejectedLineBreak = true;
    }
    reporter.check(
        rejectedLineBreak,
        "stored password lease rejects line breaks");

    std::cout << "[SUMMARY] failures=" << reporter.failures()
              << std::endl;
    return reporter.failures() == 0 ? 0 : 1;
}
