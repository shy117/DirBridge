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
    const std::vector<std::uint8_t> encoded = encodeFrame(
        {type, generation, sequence, std::move(payload)});
    return !encoded.empty() && writeAll(handle, encoded);
}

bool sendOutputEvents(
    HANDLE handle,
    std::uint32_t generation,
    std::uint32_t &sequence,
    std::vector<std::uint8_t> output)
{
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
                handle,
                FrameType::Output,
                generation,
                sequence++,
                std::move(chunk)))
        {
            return false;
        }
        offset += size;
    }
    return true;
}

enum class ReadFrameResult
{
    Ok,
    NoData,
    Eof,
    Error
};

bool readExact(HANDLE handle, std::uint8_t *bytes, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size)
    {
        DWORD read = 0;
        if (!ReadFile(
                handle,
                bytes + offset,
                static_cast<DWORD>(size - offset),
                &read,
                nullptr)
            || read == 0)
        {
            return false;
        }
        offset += read;
    }
    return true;
}

std::uint32_t frameUint32(const std::uint8_t *bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

ReadFrameResult readFrame(
    HANDLE handle,
    Frame &frame,
    std::string &error)
{
    std::array<std::uint8_t, FrameHeaderSize> header{};
    DWORD firstRead = 0;
    if (!ReadFile(handle, header.data(), header.size(), &firstRead, nullptr))
    {
        const DWORD code = GetLastError();
        if (code == ERROR_BROKEN_PIPE || code == ERROR_OPERATION_ABORTED)
        {
            return ReadFrameResult::Eof;
        }
        error = "failed to read broker frame header";
        return ReadFrameResult::Error;
    }
    if (firstRead == 0)
    {
        return ReadFrameResult::Eof;
    }
    if (firstRead < header.size()
        && !readExact(
            handle,
            header.data() + firstRead,
            header.size() - firstRead))
    {
        error = "truncated broker frame header";
        return ReadFrameResult::Error;
    }
    const std::uint32_t payloadSize = frameUint32(header.data() + 20);
    if (payloadSize > MaximumPayloadSize)
    {
        error = "broker frame payload exceeded limit";
        return ReadFrameResult::Error;
    }
    std::vector<std::uint8_t> encoded(header.begin(), header.end());
    encoded.resize(FrameHeaderSize + payloadSize);
    if (payloadSize > 0
        && !readExact(handle, encoded.data() + FrameHeaderSize, payloadSize))
    {
        error = "truncated broker frame payload";
        return ReadFrameResult::Error;
    }
    std::vector<Frame> decoded;
    if (!decodeFrames(encoded, decoded, error) || decoded.size() != 1)
    {
        return ReadFrameResult::Error;
    }
    frame = std::move(decoded.front());
    return ReadFrameResult::Ok;
}

ReadFrameResult peekFrame(
    HANDLE handle,
    Frame &frame,
    std::string &error)
{
    std::array<std::uint8_t, FrameHeaderSize> header{};
    DWORD peeked = 0;
    DWORD available = 0;
    if (!PeekNamedPipe(
            handle,
            header.data(),
            static_cast<DWORD>(header.size()),
            &peeked,
            &available,
            nullptr))
    {
        const DWORD code = GetLastError();
        if (code == ERROR_BROKEN_PIPE || code == ERROR_OPERATION_ABORTED)
        {
            return ReadFrameResult::Eof;
        }
        error = "failed to inspect broker command pipe";
        return ReadFrameResult::Error;
    }
    if (available < FrameHeaderSize || peeked < FrameHeaderSize)
    {
        return ReadFrameResult::NoData;
    }
    const std::uint32_t payloadSize = frameUint32(header.data() + 20);
    if (payloadSize > MaximumPayloadSize)
    {
        error = "broker frame payload exceeded limit";
        return ReadFrameResult::Error;
    }
    if (available < FrameHeaderSize + payloadSize)
    {
        return ReadFrameResult::NoData;
    }
    return readFrame(handle, frame, error);
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
    Frame startFrame;
    std::string error;
    if (readFrame(command, startFrame, error) != ReadFrameResult::Ok
        || startFrame.type != FrameType::Start
        || startFrame.generation == 0
        || startFrame.sequence != 1)
    {
        return fail(
            event,
            0,
            1,
            error.empty() ? "invalid broker Start frame" : error,
            65);
    }
    const std::uint32_t generation = startFrame.generation;

    StartRequest request;
    if (!decodeStartRequest(startFrame.payload, request, error))
    {
        return fail(event, generation, 1, error, 66);
    }
    const bool requiresPassword = request.ssh.authentication
        == SshAuthenticationMode::StoredPassword;
    Frame secretFrame;
    std::uint32_t nextCommandSequence = 2;
    if (requiresPassword)
    {
        if (readFrame(command, secretFrame, error) != ReadFrameResult::Ok
            || secretFrame.type != FrameType::AuthSecret
            || secretFrame.generation != generation
            || secretFrame.sequence != nextCommandSequence
            || secretFrame.payload.empty())
        {
            return fail(
                event,
                generation,
                1,
                error.empty() ? "invalid broker AuthSecret frame" : error,
                67);
        }
        ++nextCommandSequence;
    }

    OpenSshLauncherConfig launcherConfig;
    launcherConfig.confirmedExecutablePath = request.sshExecutable;
    launcherConfig.workingDirectory = request.workingDirectory;
    OpenSshLaunchResult launch = OpenSshLauncher::build(
        request.ssh,
        launcherConfig);
    if (!launch.spec)
    {
        return fail(event, generation, 1, launch.error, 68);
    }

    std::unique_ptr<StoredPasswordChannel> passwordChannel;
    std::vector<std::pair<std::wstring, std::wstring>> environment;
    if (requiresPassword)
    {
        const std::string_view password(
            reinterpret_cast<const char *>(secretFrame.payload.data()),
            secretFrame.payload.size());
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
        SecureZeroMemory(secretFrame.payload.data(), secretFrame.payload.size());
        secretFrame.payload.clear();
        if (!passwordChannel)
        {
            return fail(event, generation, 1, error, 69);
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
        return fail(event, generation, 1, process.error(), 70);
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
    if (!sendEvent(event, FrameType::Ready, generation, eventSequence++))
    {
        process.terminate(71);
        if (passwordChannel)
        {
            passwordChannel->cancel();
        }
        if (passwordThread.joinable())
        {
            passwordThread.join();
        }
        return 71;
    }

    bool closeRequested = false;
    bool commandFailed = false;
    bool commandOpen = true;
    std::string commandError;
    std::uint32_t expectedCommandSequence = nextCommandSequence;

    auto closeStarted = std::chrono::steady_clock::time_point{};
    bool eventWriteFailed = false;
    bool forcedClose = false;
    while (!process.hasExited() && !commandFailed)
    {
        while (commandOpen && !closeRequested)
        {
            Frame frame;
            std::string readError;
            const ReadFrameResult result = peekFrame(command, frame, readError);
            if (result == ReadFrameResult::NoData)
            {
                break;
            }
            if (result == ReadFrameResult::Eof)
            {
                commandOpen = false;
                break;
            }
            if (result != ReadFrameResult::Ok
                || frame.generation != generation
                || frame.sequence != expectedCommandSequence++)
            {
                commandError = readError.empty()
                    ? "invalid streaming broker command sequence"
                    : readError;
                commandFailed = true;
                break;
            }
            if (frame.type == FrameType::Input)
            {
                if (!process.send(frame.payload))
                {
                    commandError = process.error();
                    commandFailed = true;
                    break;
                }
            }
            else if (frame.type == FrameType::Resize)
            {
                if (frame.payload.size() != 4)
                {
                    commandError = "Resize frame payload must be four bytes";
                    commandFailed = true;
                    break;
                }
                const std::uint16_t columns = static_cast<std::uint16_t>(
                    frame.payload[0] | frame.payload[1] << 8U);
                const std::uint16_t rows = static_cast<std::uint16_t>(
                    frame.payload[2] | frame.payload[3] << 8U);
                if (!process.resize(columns, rows))
                {
                    commandError = process.error();
                    commandFailed = true;
                    break;
                }
            }
            else if (frame.type == FrameType::Close)
            {
                closeRequested = true;
                process.closeInput();
            }
            else
            {
                commandError = "command is not valid after Ready";
                commandFailed = true;
                break;
            }
        }
        std::vector<std::uint8_t> output = process.takeOutput();
        if (!sendOutputEvents(
                event,
                generation,
                eventSequence,
                std::move(output)))
        {
            eventWriteFailed = true;
            break;
        }
        if (closeRequested)
        {
            if (closeStarted == std::chrono::steady_clock::time_point{})
            {
                closeStarted = std::chrono::steady_clock::now();
            }
            else if (std::chrono::steady_clock::now() - closeStarted
                >= std::chrono::seconds(2))
            {
                process.terminate(72);
                forcedClose = true;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (commandFailed || eventWriteFailed)
    {
        process.terminate(73);
    }
    std::uint32_t sshExitCode = commandFailed || eventWriteFailed
        ? 73
        : (forcedClose ? 72 : 0);
    if (!commandFailed && !eventWriteFailed && !forcedClose && !process.wait(
            std::chrono::seconds(2),
            sshExitCode))
    {
        process.terminate(74);
        commandFailed = true;
        commandError = process.error();
    }
    if (passwordChannel)
    {
        passwordChannel->cancel();
    }
    if (passwordThread.joinable())
    {
        passwordThread.join();
    }

    std::vector<std::uint8_t> finalOutput = process.takeOutput();
    if (!sendOutputEvents(
            event,
            generation,
            eventSequence,
            std::move(finalOutput)))
    {
        return 75;
    }
    if (commandFailed)
    {
        return fail(event, generation, eventSequence, commandError, 76);
    }
    if (eventWriteFailed)
    {
        return 77;
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
            78);
    }
    sendEvent(
        event,
        FrameType::Exit,
        generation,
        eventSequence++,
        exitPayload(sshExitCode));
    sendEvent(event, FrameType::Stopped, generation, eventSequence++);
    return sshExitCode == 0 ? 0 : 79;
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
