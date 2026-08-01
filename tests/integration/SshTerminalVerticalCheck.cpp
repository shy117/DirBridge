#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "config/SiteStore.h"
#include "terminal/TerminalBrokerClient.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace dirbridge::terminal;
using namespace dirbridge::terminal::broker;

std::uint32_t read32(const std::uint8_t *bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

std::optional<std::wstring> argument(
    int count,
    wchar_t **values,
    const wchar_t *name)
{
    for (int index = 1; index + 1 < count; ++index)
    {
        if (std::wstring(values[index]) == name)
        {
            return values[index + 1];
        }
    }
    return std::nullopt;
}

int run(
    const std::filesystem::path &broker,
    const std::filesystem::path &askPass,
    const std::filesystem::path &config,
    const std::string &siteName)
{
    std::vector<SiteProfile> sites = SiteStore(config).load();
    auto site = std::find_if(
        sites.begin(), sites.end(), [&siteName](const SiteProfile &value) {
            return value.name == siteName;
        });
    if (site == sites.end()
        || site->protocol != RemoteProtocol::Sftp
        || site->password.empty())
    {
        std::cerr << "saved SFTP site with password was not found\n";
        return 2;
    }

    wchar_t windowsDirectory[32768]{};
    const UINT windowsLength = GetWindowsDirectoryW(
        windowsDirectory,
        static_cast<UINT>(std::size(windowsDirectory)));
    if (windowsLength == 0 || windowsLength >= std::size(windowsDirectory))
    {
        return 3;
    }
    StartRequest start;
    start.ssh.displayName = site->name;
    start.ssh.host = site->host;
    start.ssh.port = site->port;
    start.ssh.username = site->username;
    start.ssh.authentication = SshAuthenticationMode::StoredPassword;
    start.sshExecutable = std::filesystem::path(windowsDirectory)
        / L"System32" / L"OpenSSH" / L"ssh.exe";
    start.workingDirectory = std::filesystem::current_path();
    start.askPassHelper = askPass;

    TerminalBrokerClient client;
    StoredPasswordLease password(site->password);
    SecureZeroMemory(site->password.data(), site->password.size());
    site->password.clear();
    if (!client.start(broker, start, std::move(password)))
    {
        std::cerr << client.error() << '\n';
        return 4;
    }

    std::vector<Frame> events;
    std::vector<std::uint8_t> output;
    bool sentRuntimeCommands = false;
    bool sentClose = false;
    bool brokerError = false;
    bool exited = false;
    std::uint32_t sshExitCode = 0;
    bool stopped = false;
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::seconds(60);
    while (!stopped && std::chrono::steady_clock::now() < deadline)
    {
        Frame frame;
        if (client.readEvent(frame) != EventReadResult::Event)
        {
            std::cerr << client.error() << '\n';
            return 5;
        }
        events.push_back(frame);
        if (frame.type == FrameType::Ready && !sentRuntimeCommands)
        {
            const std::string input =
                "printf 'DIRBRIDGE_SSH_UBUNTU_OK:%s\\n' \"$(uname -s)\"\r";
            if (!client.resize(120, 40)
                || !client.sendInput(std::vector<std::uint8_t>(
                    input.begin(), input.end())))
            {
                std::cerr << client.error() << '\n';
                return 6;
            }
            sentRuntimeCommands = true;
        }
        if (frame.type == FrameType::Error)
        {
            std::cerr << "broker error: "
                      << std::string(frame.payload.begin(), frame.payload.end())
                      << '\n';
            brokerError = true;
        }
        else if (frame.type == FrameType::Output)
        {
            output.insert(output.end(), frame.payload.begin(), frame.payload.end());
            const std::string marker = "DIRBRIDGE_SSH_UBUNTU_OK:Linux";
            if (!sentClose && std::search(
                    output.begin(), output.end(), marker.begin(), marker.end())
                    != output.end())
            {
                if (!client.close())
                {
                    std::cerr << client.error() << '\n';
                    return 7;
                }
                sentClose = true;
            }
        }
        else if (frame.type == FrameType::Exit)
        {
            exited = frame.payload.size() == 4;
            if (exited)
            {
                sshExitCode = read32(frame.payload.data());
            }
        }
        else if (frame.type == FrameType::Stopped)
        {
            stopped = true;
        }
    }

    std::uint32_t brokerExitCode = 0;
    if (!stopped || !client.waitForBroker(
            std::chrono::seconds(5), brokerExitCode))
    {
        std::cerr << client.error() << '\n';
        return 8;
    }
    const std::string marker = "DIRBRIDGE_SSH_UBUNTU_OK:Linux";
    const bool connected = std::search(
        output.begin(), output.end(), marker.begin(), marker.end()) != output.end();
    const bool passed = connected && sentClose && exited && stopped && !brokerError;
    std::cout << (passed ? "[PASS] " : "[FAIL] ")
              << "Broker client + ConPTY + OpenSSH + Ubuntu\n";
    std::cout << "[SUMMARY] events=" << events.size()
              << " output_bytes=" << output.size()
              << " ssh_exit_code=" << sshExitCode
              << " broker_exit_code=" << brokerExitCode << '\n';
    return passed ? 0 : 9;
}

} // namespace

int main()
{
    int count = 0;
    wchar_t **values = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!values)
    {
        return 64;
    }
    const auto broker = argument(count, values, L"--broker");
    const auto askPass = argument(count, values, L"--askpass");
    const auto config = argument(count, values, L"--config");
    const auto site = argument(count, values, L"--site");
    if (!broker || !askPass || !config || !site)
    {
        LocalFree(values);
        std::cerr << "missing vertical-check arguments\n";
        return 64;
    }
    const std::wstring siteWide = *site;
    const int utf8Size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, siteWide.data(), siteWide.size(),
        nullptr, 0, nullptr, nullptr);
    std::string siteUtf8(static_cast<std::size_t>(utf8Size), '\0');
    WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, siteWide.data(), siteWide.size(),
        siteUtf8.data(), utf8Size, nullptr, nullptr);
    std::error_code pathError;
    const std::filesystem::path brokerPath = std::filesystem::absolute(
        std::filesystem::path(*broker), pathError);
    const std::filesystem::path askPassPath = std::filesystem::absolute(
        std::filesystem::path(*askPass), pathError);
    const std::filesystem::path configPath = std::filesystem::absolute(
        std::filesystem::path(*config), pathError);
    LocalFree(values);
    if (pathError)
    {
        std::cerr << "failed to resolve vertical-check paths\n";
        return 65;
    }
    return run(brokerPath, askPassPath, configPath, siteUtf8);
}
