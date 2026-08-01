#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "terminal/AskPassProtocol.h"
#include "terminal/StoredPasswordLease.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace {

using dirbridge::terminal::StoredPasswordLease;
using namespace dirbridge::terminal::askpass;

constexpr wchar_t PipeEnvironment[] = L"DIRBRIDGE_SSH_ASKPASS_PIPE";
constexpr wchar_t TokenEnvironment[] = L"DIRBRIDGE_SSH_ASKPASS_TOKEN";
constexpr wchar_t PromptEnvironment[] = L"SSH_ASKPASS_PROMPT";
constexpr wchar_t PipePrefix[] = L"\\\\.\\pipe\\DirBridge.SshAskPass.";

std::wstring readEnvironment(const wchar_t *name)
{
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0)
    {
        return {};
    }
    std::wstring value(required, L'\0');
    const DWORD written = GetEnvironmentVariableW(
        name,
        value.data(),
        static_cast<DWORD>(value.size()));
    if (written == 0 || written >= value.size())
    {
        return {};
    }
    value.resize(written);
    return value;
}

bool isHexToken(const std::wstring &token)
{
    if (token.size() < MinTokenBytes || token.size() > MaxTokenBytes)
    {
        return false;
    }
    return std::all_of(token.begin(), token.end(), [](wchar_t character) {
        return (character >= L'0' && character <= L'9')
            || (character >= L'a' && character <= L'f')
            || (character >= L'A' && character <= L'F');
    });
}

void put16(std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    for (int shift = 0; shift < 32; shift += 8)
    {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint16_t get16(const std::uint8_t *bytes)
{
    return static_cast<std::uint16_t>(bytes[0])
        | (static_cast<std::uint16_t>(bytes[1]) << 8U);
}

std::uint32_t get32(const std::uint8_t *bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

bool writeAll(HANDLE handle, const void *data, std::size_t size)
{
    const auto *cursor = static_cast<const std::uint8_t *>(data);
    while (size > 0)
    {
        const DWORD part = static_cast<DWORD>(std::min<std::size_t>(
            size,
            64U * 1024U));
        DWORD written = 0;
        if (!WriteFile(handle, cursor, part, &written, nullptr)
            || written == 0)
        {
            return false;
        }
        cursor += written;
        size -= written;
    }
    return true;
}

bool readAll(HANDLE handle, void *data, std::size_t size)
{
    auto *cursor = static_cast<std::uint8_t *>(data);
    while (size > 0)
    {
        const DWORD part = static_cast<DWORD>(std::min<std::size_t>(
            size,
            64U * 1024U));
        DWORD received = 0;
        if (!ReadFile(handle, cursor, part, &received, nullptr)
            || received == 0)
        {
            return false;
        }
        cursor += received;
        size -= received;
    }
    return true;
}

int runAskPass()
{
    // OpenSSH sets this to "confirm" for host-key/permission questions and
    // "none" for notifications. Neither may consume a stored password.
    if (!readEnvironment(PromptEnvironment).empty())
    {
        return 2;
    }

    const std::wstring pipeName = readEnvironment(PipeEnvironment);
    const std::wstring token = readEnvironment(TokenEnvironment);
    if (pipeName.rfind(PipePrefix, 0) != 0 || !isHexToken(token))
    {
        return 3;
    }
    if (!WaitNamedPipeW(pipeName.c_str(), 10000))
    {
        return 4;
    }

    HANDLE pipe = CreateFileW(
        pipeName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE)
    {
        return 5;
    }

    std::vector<std::uint8_t> request;
    request.reserve(HeaderSize + token.size());
    put32(request, Magic);
    put16(request, Version);
    put16(request, 0);
    put32(request, static_cast<std::uint32_t>(token.size()));
    for (const wchar_t character : token)
    {
        request.push_back(static_cast<std::uint8_t>(character));
    }
    if (!writeAll(pipe, request.data(), request.size()))
    {
        CloseHandle(pipe);
        return 6;
    }

    std::array<std::uint8_t, HeaderSize> header{};
    if (!readAll(pipe, header.data(), header.size()))
    {
        CloseHandle(pipe);
        return 7;
    }
    const std::uint32_t magic = get32(header.data());
    const std::uint16_t version = get16(header.data() + 4);
    const auto status = static_cast<ResponseStatus>(get16(header.data() + 6));
    const std::uint32_t passwordSize = get32(header.data() + 8);
    if (magic != Magic
        || version != Version
        || status != ResponseStatus::Password
        || passwordSize > StoredPasswordLease::MaxPasswordBytes)
    {
        CloseHandle(pipe);
        return 8;
    }

    std::vector<char> password(passwordSize);
    const bool received = password.empty()
        || readAll(pipe, password.data(), password.size());
    CloseHandle(pipe);
    if (!received)
    {
        if (!password.empty())
        {
            SecureZeroMemory(password.data(), password.size());
        }
        return 9;
    }

    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    const char newline = '\n';
    const bool written = output != nullptr
        && output != INVALID_HANDLE_VALUE
        && (password.empty()
            || writeAll(output, password.data(), password.size()))
        && writeAll(output, &newline, 1);
    if (!password.empty())
    {
        SecureZeroMemory(password.data(), password.size());
    }
    return written ? 0 : 10;
}

} // namespace

int main()
{
    return runAskPass();
}
