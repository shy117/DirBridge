#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "terminal/TerminalBrokerClient.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

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

std::uint32_t read32(const std::uint8_t *bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool readFrame(HANDLE handle, Frame &frame)
{
    std::array<std::uint8_t, FrameHeaderSize> header{};
    if (!readExact(handle, header.data(), header.size()))
    {
        return false;
    }
    const std::uint32_t payloadSize = read32(header.data() + 20);
    if (payloadSize > MaximumPayloadSize)
    {
        return false;
    }
    std::vector<std::uint8_t> encoded(header.begin(), header.end());
    encoded.resize(FrameHeaderSize + payloadSize);
    if (payloadSize > 0 && !readExact(
            handle,
            encoded.data() + FrameHeaderSize,
            payloadSize))
    {
        return false;
    }
    std::vector<Frame> decoded;
    std::string error;
    if (!decodeFrames(encoded, decoded, error) || decoded.size() != 1)
    {
        return false;
    }
    frame = std::move(decoded.front());
    return true;
}

bool writeFrame(HANDLE handle, Frame frame)
{
    const std::vector<std::uint8_t> encoded = encodeFrame(frame);
    return !encoded.empty() && writeAll(handle, encoded);
}

std::optional<HANDLE> inheritedHandle(
    int count,
    wchar_t **values,
    const wchar_t *name)
{
    for (int index = 1; index + 1 < count; ++index)
    {
        if (std::wstring(values[index]) != name)
        {
            continue;
        }
        try
        {
            const auto value = std::stoull(values[index + 1]);
            return reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value));
        }
        catch (...)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

int runFakeBroker(HANDLE command, HANDLE event)
{
    Frame start;
    Frame secret;
    Frame resize;
    Frame input;
    Frame close;
    if (!readFrame(command, start)
        || start.type != FrameType::Start
        || start.sequence != 1)
    {
        return 10;
    }
    StartRequest request;
    std::string error;
    if (!decodeStartRequest(start.payload, request, error))
    {
        return 10;
    }
    if (!request.ssh.allowLegacySshRsaHostKey)
    {
        return 10;
    }
    std::uint32_t nextCommandSequence = 2;
    if (request.ssh.authentication
        == dirbridge::terminal::SshAuthenticationMode::StoredPassword)
    {
        if (!readFrame(command, secret)
            || secret.type != FrameType::AuthSecret
            || secret.sequence != nextCommandSequence
            || secret.payload.empty())
        {
            return 10;
        }
        ++nextCommandSequence;
    }
    if (request.ssh.host == "host-key-conflict.invalid")
    {
        const std::string message =
            "WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!\r\n"
            "The fingerprint for the ED25519 key sent by the remote host is\r\n"
            "SHA256:DirBridgeHostKeyConflictTest=\r\n"
            "Host key verification failed.\r\n";
        return writeFrame(event, {FrameType::Ready, start.generation, 1, {}})
                && writeFrame(event, {FrameType::Output, start.generation, 2,
                    std::vector<std::uint8_t>(message.begin(), message.end())})
                && writeFrame(event, {FrameType::Exit, start.generation, 3, {255, 0, 0, 0}})
                && writeFrame(event, {FrameType::Stopped, start.generation, 4, {}})
            ? 0
            : 11;
    }
    if (!writeFrame(event, {FrameType::Ready, start.generation, 1, {}})
        || !readFrame(command, resize)
        || resize.type != FrameType::Resize
        || resize.sequence != nextCommandSequence++
        || resize.payload != std::vector<std::uint8_t>({100, 0, 30, 0})
        || !readFrame(command, input)
        || input.type != FrameType::Input
        || input.sequence != nextCommandSequence++
        || input.payload != std::vector<std::uint8_t>({'p', 'w', 'd', '\r'}))
    {
        return 10;
    }
    if (!writeFrame(
            event,
            {FrameType::Output, start.generation, 2, {'o', 'k'}})
        || !readFrame(command, close)
        || close.type != FrameType::Close
        || close.sequence != nextCommandSequence)
    {
        return 10;
    }
    return writeFrame(
                event,
                {FrameType::Exit, start.generation, 3, {0, 0, 0, 0}})
            && writeFrame(
                event,
                {FrameType::Stopped, start.generation, 4, {}})
        ? 0
        : 11;
}

int runParent(const std::filesystem::path &self)
{
    StartRequest request;
    request.ssh.displayName = "fake";
    request.ssh.host = "example.invalid";
    request.ssh.authentication = dirbridge::terminal::SshAuthenticationMode::SystemDefault;
    request.ssh.allowLegacySshRsaHostKey = true;

    TerminalBrokerClient invalidClient;
    StartRequest passwordRequest = request;
    passwordRequest.ssh.authentication =
        dirbridge::terminal::SshAuthenticationMode::StoredPassword;
    if (invalidClient.start(self, passwordRequest)
        || invalidClient.error().empty())
    {
        return 1;
    }

    TerminalBrokerClient client;
    if (!client.start(self, request))
    {
        std::cerr << client.error() << '\n';
        return 2;
    }
    Frame ready;
    if (client.readEvent(ready) != EventReadResult::Event
        || ready.type != FrameType::Ready
        || !client.resize(100, 30)
        || !client.sendInput({'p', 'w', 'd', '\r'})
        || !client.close())
    {
        std::cerr << client.error() << '\n';
        return 3;
    }
    Frame output;
    Frame exited;
    Frame stopped;
    if (client.readEvent(output) != EventReadResult::Event
        || client.readEvent(exited) != EventReadResult::Event
        || client.readEvent(stopped) != EventReadResult::Event
        || output.type != FrameType::Output
        || output.payload != std::vector<std::uint8_t>({'o', 'k'})
        || exited.type != FrameType::Exit
        || stopped.type != FrameType::Stopped)
    {
        std::cerr << client.error() << '\n';
        return 4;
    }
    std::uint32_t exitCode = 0;
    if (!client.waitForBroker(std::chrono::seconds(5), exitCode)
        || exitCode != 0)
    {
        std::cerr << client.error() << '\n';
        return 5;
    }
    std::cout << "[PASS] production broker client framing and lifecycle\n";
    return 0;
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
    const auto command = inheritedHandle(
        count, values, L"--command-handle");
    const auto event = inheritedHandle(
        count, values, L"--event-handle");
    if (command || event)
    {
        if (!command || !event)
        {
            LocalFree(values);
            return 64;
        }
        Handle commandOwner(*command);
        Handle eventOwner(*event);
        LocalFree(values);
        return runFakeBroker(commandOwner.get(), eventOwner.get());
    }

    wchar_t modulePath[32768]{};
    const DWORD length = GetModuleFileNameW(
        nullptr, modulePath, static_cast<DWORD>(std::size(modulePath)));
    LocalFree(values);
    if (length == 0 || length >= std::size(modulePath))
    {
        return 65;
    }
    return runParent(std::filesystem::path(
        std::wstring(modulePath, modulePath + length)));
}
