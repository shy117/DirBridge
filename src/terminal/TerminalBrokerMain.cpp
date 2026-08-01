#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "terminal/ConPtyProcess.h"
#include "terminal/OpenSshLauncher.h"
#include "terminal/StoredPasswordChannel.h"
#include "terminal/TerminalBrokerProtocol.h"
#include "terminal/TerminalBrokerStart.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace dirbridge::terminal;
using namespace dirbridge::terminal::broker;

class Handle
{
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle()
    {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value_);
        }
    }
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    HANDLE get() const noexcept { return value_; }

private:
    HANDLE value_ = nullptr;
};

std::wstring optionValue(
    int count,
    wchar_t **arguments,
    const wchar_t *option)
{
    for (int index = 1; index + 1 < count; ++index)
    {
        if (std::wstring(arguments[index]) == option)
        {
            return arguments[index + 1];
        }
    }
    return {};
}

std::optional<HANDLE> inheritedHandle(
    int count,
    wchar_t **arguments,
    const wchar_t *option)
{
    const std::wstring value = optionValue(count, arguments, option);
    try
    {
        std::size_t parsed = 0;
        const unsigned long long numeric = std::stoull(value, &parsed, 10);
        if (parsed != value.size() || numeric == 0)
        {
            return std::nullopt;
        }
        return reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(numeric));
    }
    catch (...)
    {
        return std::nullopt;
    }
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

bool sendEvent(
    HANDLE handle,
    FrameType type,
    std::uint32_t generation,
    std::uint32_t sequence,
    std::vector<std::uint8_t> payload = {})
{
    return writeAll(handle, encodeFrame(
        {type, generation, sequence, std::move(payload)}));
}

bool readCommands(HANDLE handle, std::vector<std::uint8_t> &bytes)
{
    std::array<std::uint8_t, 4096> buffer{};
    constexpr std::size_t MaximumStreamSize = 2U * 1024U * 1024U;
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
        if (bytes.size() > MaximumStreamSize - read)
        {
            return false;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + read);
    }
}

std::vector<std::uint8_t> exitPayload(std::uint32_t exitCode)
{
    return {
        static_cast<std::uint8_t>(exitCode),
        static_cast<std::uint8_t>(exitCode >> 8U),
        static_cast<std::uint8_t>(exitCode >> 16U),
        static_cast<std::uint8_t>(exitCode >> 24U)};
}

int fail(
    HANDLE event,
    std::uint32_t generation,
    std::uint32_t sequence,
    const std::string &message,
    int exitCode)
{
    const std::vector<std::uint8_t> payload(message.begin(), message.end());
    sendEvent(event, FrameType::Error, generation, sequence, payload);
    return exitCode;
}

int runBroker(HANDLE command, HANDLE event)
{
    std::vector<std::uint8_t> commandBytes;
    if (!readCommands(command, commandBytes))
    {
        return fail(event, 0, 1, "failed to read broker commands", 65);
    }
    std::vector<Frame> frames;
    std::string error;
    if (!decodeFrames(commandBytes, frames, error)
        || !validateCommandSequence(frames, error))
    {
        SecureZeroMemory(commandBytes.data(), commandBytes.size());
        return fail(event, 0, 1, error, 66);
    }
    SecureZeroMemory(commandBytes.data(), commandBytes.size());
    const std::uint32_t generation = frames.front().generation;

    StartRequest request;
    if (!decodeStartRequest(frames.front().payload, request, error))
    {
        return fail(event, generation, 1, error, 67);
    }
    Frame *secretFrame = nullptr;
    std::vector<std::uint8_t> initialInput;
    for (Frame &frame : frames)
    {
        if (frame.type == FrameType::AuthSecret)
        {
            secretFrame = &frame;
        }
        else if (frame.type == FrameType::Input)
        {
            initialInput.insert(
                initialInput.end(),
                frame.payload.begin(),
                frame.payload.end());
        }
    }
    const bool requiresPassword = request.ssh.authentication
        == SshAuthenticationMode::StoredPassword;
    if (requiresPassword != (secretFrame != nullptr))
    {
        return fail(
            event,
            generation,
            1,
            "stored-password mode and AuthSecret frame do not match",
            68);
    }

    OpenSshLauncherConfig launcherConfig;
    launcherConfig.confirmedExecutablePath = request.sshExecutable;
    launcherConfig.workingDirectory = request.workingDirectory;
    OpenSshLaunchResult launch = OpenSshLauncher::build(
        request.ssh,
        launcherConfig);
    if (!launch.spec)
    {
        return fail(event, generation, 1, launch.error, 69);
    }

    std::unique_ptr<StoredPasswordChannel> passwordChannel;
    std::vector<std::pair<std::wstring, std::wstring>> environment;
    if (secretFrame != nullptr)
    {
        const std::string_view password(
            reinterpret_cast<const char *>(secretFrame->payload.data()),
            secretFrame->payload.size());
        try
        {
            passwordChannel = StoredPasswordChannel::create(
                StoredPasswordLease(password),
                request.askPassHelper,
                error);
        }
        catch (const std::exception &exception)
        {
            error = exception.what();
        }
        SecureZeroMemory(
            secretFrame->payload.data(),
            secretFrame->payload.size());
        secretFrame = nullptr;
        if (!passwordChannel)
        {
            return fail(event, generation, 1, error, 70);
        }
        environment = {
            {L"SSH_ASKPASS", request.askPassHelper.wstring()},
            {L"SSH_ASKPASS_REQUIRE", L"force"},
            {L"DIRBRIDGE_SSH_ASKPASS_PIPE", passwordChannel->endpoint().pipeName},
            {L"DIRBRIDGE_SSH_ASKPASS_TOKEN", passwordChannel->endpoint().token},
        };
    }

    ConPtyProcess process;
    if (!process.start(*launch.spec, environment, 100, 30))
    {
        return fail(event, generation, 1, process.error(), 71);
    }

    bool passwordServed = !passwordChannel;
    std::string passwordError;
    std::thread passwordThread;
    if (passwordChannel)
    {
        passwordThread = std::thread([&] {
            passwordServed = passwordChannel->serveOnce(passwordError);
        });
    }

    std::uint32_t eventSequence = 1;
    sendEvent(event, FrameType::Ready, generation, eventSequence++);
    if (!initialInput.empty() && !process.send(initialInput))
    {
        process.terminate(72);
        if (passwordChannel)
        {
            passwordChannel->cancel();
        }
        if (passwordThread.joinable())
        {
            passwordThread.join();
        }
        return fail(event, generation, eventSequence, process.error(), 72);
    }

    std::uint32_t sshExitCode = 0;
    if (!process.wait(std::chrono::seconds(45), sshExitCode))
    {
        process.terminate(73);
        if (passwordChannel)
        {
            passwordChannel->cancel();
        }
        if (passwordThread.joinable())
        {
            passwordThread.join();
        }
        return fail(event, generation, eventSequence, process.error(), 73);
    }
    if (passwordChannel)
    {
        passwordChannel->cancel();
    }
    if (passwordThread.joinable())
    {
        passwordThread.join();
    }

    std::vector<std::uint8_t> output = process.takeOutput();
    std::size_t offset = 0;
    while (offset < output.size())
    {
        const std::size_t size = std::min<std::size_t>(
            MaximumPayloadSize,
            output.size() - offset);
        std::vector<std::uint8_t> chunk(
            output.begin() + static_cast<std::ptrdiff_t>(offset),
            output.begin() + static_cast<std::ptrdiff_t>(offset + size));
        if (!sendEvent(
                event,
                FrameType::Output,
                generation,
                eventSequence++,
                std::move(chunk)))
        {
            return 74;
        }
        offset += size;
    }
    if (!passwordServed && requiresPassword)
    {
        return fail(
            event,
            generation,
            eventSequence,
            passwordError.empty()
                ? "stored password was not consumed"
                : passwordError,
            75);
    }
    sendEvent(
        event,
        FrameType::Exit,
        generation,
        eventSequence++,
        exitPayload(sshExitCode));
    sendEvent(event, FrameType::Stopped, generation, eventSequence++);
    return sshExitCode == 0 ? 0 : 76;
}

} // namespace

int main()
{
    int count = 0;
    wchar_t **arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr)
    {
        return 64;
    }
    const auto command = inheritedHandle(
        count, arguments, L"--command-handle");
    const auto event = inheritedHandle(
        count, arguments, L"--event-handle");
    LocalFree(arguments);
    if (!command || !event)
    {
        std::cerr << "invalid broker inherited handles\n";
        return 64;
    }
    Handle commandOwner(*command);
    Handle eventOwner(*event);
    return runBroker(commandOwner.get(), eventOwner.get());
}
