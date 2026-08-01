#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "config/SiteStore.h"
#include "terminal/TerminalBrokerProtocol.h"
#include "terminal/TerminalBrokerStart.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002
#endif

namespace {

using namespace dirbridge::terminal;
using namespace dirbridge::terminal::broker;

class Handle
{
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE get() const noexcept { return value_; }
    explicit operator bool() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    void reset(HANDLE value = nullptr) noexcept
    {
        if (*this)
        {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_ = nullptr;
};

std::wstring quote(const std::wstring &value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t character : value)
    {
        if (character == L'\\')
        {
            ++slashes;
        }
        else if (character == L'\"')
        {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(character);
            slashes = 0;
        }
        else
        {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(character);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool writeAll(HANDLE handle, const std::vector<std::uint8_t> &bytes)
{
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        DWORD written = 0;
        const DWORD size = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset,
            std::numeric_limits<DWORD>::max()));
        if (!WriteFile(handle, bytes.data() + offset, size, &written, nullptr)
            || written == 0)
        {
            return false;
        }
        offset += written;
    }
    return true;
}

bool readToEnd(HANDLE handle, std::vector<std::uint8_t> &bytes)
{
    std::array<std::uint8_t, 4096> buffer{};
    for (;;)
    {
        DWORD read = 0;
        if (!ReadFile(handle, buffer.data(), buffer.size(), &read, nullptr))
        {
            return GetLastError() == ERROR_BROKEN_PIPE;
        }
        if (read == 0)
        {
            return true;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + read);
    }
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

    constexpr std::uint32_t Generation = 1;
    Frame startFrame{FrameType::Start, Generation, 1, encodeStartRequest(start)};
    Frame secretFrame{
        FrameType::AuthSecret,
        Generation,
        2,
        std::vector<std::uint8_t>(site->password.begin(), site->password.end())};
    SecureZeroMemory(site->password.data(), site->password.size());
    site->password.clear();
    const std::string input =
        "printf 'DIRBRIDGE_SSH_UBUNTU_OK:%s\\n' \"$(uname -s)\"\rexit\r";
    Frame inputFrame{
        FrameType::Input,
        Generation,
        3,
        std::vector<std::uint8_t>(input.begin(), input.end())};

    std::vector<std::uint8_t> commands;
    for (const Frame *frame : {&startFrame, &secretFrame, &inputFrame})
    {
        const auto encoded = encodeFrame(*frame);
        commands.insert(commands.end(), encoded.begin(), encoded.end());
    }
    SecureZeroMemory(secretFrame.payload.data(), secretFrame.payload.size());
    secretFrame.payload.clear();

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE commandReadRaw = nullptr;
    HANDLE commandWriteRaw = nullptr;
    HANDLE eventReadRaw = nullptr;
    HANDLE eventWriteRaw = nullptr;
    if (!CreatePipe(
            &commandReadRaw, &commandWriteRaw, &inheritable, 0)
        || !CreatePipe(
            &eventReadRaw, &eventWriteRaw, &inheritable, 0))
    {
        SecureZeroMemory(commands.data(), commands.size());
        return 4;
    }
    Handle commandRead(commandReadRaw);
    Handle commandWrite(commandWriteRaw);
    Handle eventRead(eventReadRaw);
    Handle eventWrite(eventWriteRaw);
    SetHandleInformation(commandWrite.get(), HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(eventRead.get(), HANDLE_FLAG_INHERIT, 0);

    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(
            job.get(), JobObjectExtendedLimitInformation, &limit, sizeof(limit)))
    {
        SecureZeroMemory(commands.data(), commands.size());
        return 5;
    }

    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<std::uint8_t> storage(attributeSize);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize))
    {
        SecureZeroMemory(commands.data(), commands.size());
        return 6;
    }
    struct Guard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~Guard() { DeleteProcThreadAttributeList(value); }
    } guard{attributes};
    HANDLE inherited[] = {commandRead.get(), eventWrite.get()};
    if (!UpdateProcThreadAttribute(
            attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited, sizeof(inherited), nullptr, nullptr))
    {
        SecureZeroMemory(commands.data(), commands.size());
        return 7;
    }

    std::wstring commandLine = quote(broker.wstring())
        + L" --command-handle "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(commandRead.get()))
        + L" --event-handle "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(eventWrite.get()));
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            broker.c_str(), mutableCommand.data(), nullptr, nullptr, TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT
                | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, nullptr, &startup.StartupInfo, &process))
    {
        SecureZeroMemory(commands.data(), commands.size());
        return 8;
    }
    Handle processHandle(process.hProcess);
    Handle threadHandle(process.hThread);
    if (!AssignProcessToJobObject(job.get(), processHandle.get())
        || ResumeThread(threadHandle.get()) == static_cast<DWORD>(-1))
    {
        TerminateJobObject(job.get(), 9);
        SecureZeroMemory(commands.data(), commands.size());
        return 9;
    }
    threadHandle.reset();
    commandRead.reset();
    eventWrite.reset();
    const bool written = writeAll(commandWrite.get(), commands);
    SecureZeroMemory(commands.data(), commands.size());
    commandWrite.reset();
    if (!written)
    {
        TerminateJobObject(job.get(), 10);
        return 10;
    }
    if (WaitForSingleObject(processHandle.get(), 60000) != WAIT_OBJECT_0)
    {
        TerminateJobObject(job.get(), 11);
        return 11;
    }

    std::vector<std::uint8_t> eventBytes;
    if (!readToEnd(eventRead.get(), eventBytes))
    {
        return 12;
    }
    std::vector<Frame> events;
    std::string error;
    if (!decodeFrames(eventBytes, events, error))
    {
        std::cerr << error << '\n';
        return 13;
    }
    std::vector<std::uint8_t> output;
    for (const Frame &frame : events)
    {
        if (frame.type == FrameType::Error)
        {
            std::cerr << "broker error: "
                      << std::string(frame.payload.begin(), frame.payload.end())
                      << '\n';
        }
        else if (frame.type == FrameType::Output)
        {
            output.insert(output.end(), frame.payload.begin(), frame.payload.end());
        }
    }
    const std::string marker = "DIRBRIDGE_SSH_UBUNTU_OK:Linux";
    const bool connected = std::search(
        output.begin(), output.end(), marker.begin(), marker.end()) != output.end();
    std::cout << (connected ? "[PASS] " : "[FAIL] ")
              << "Broker + ConPTY + OpenSSH + Ubuntu\n";
    std::cout << "[SUMMARY] events=" << events.size()
              << " output_bytes=" << output.size() << '\n';
    return connected ? 0 : 14;
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
