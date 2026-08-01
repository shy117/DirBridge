#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "terminal/TerminalBrokerStart.h"

#include <cstdint>
#include <limits>
#include <optional>

namespace dirbridge::terminal::broker {
namespace {

constexpr std::uint16_t StartVersion = 1;
constexpr std::size_t MaximumStringBytes = 32768;

void put16(std::vector<std::uint8_t> &bytes, std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void put32(std::vector<std::uint8_t> &bytes, std::uint32_t value)
{
    for (unsigned shift = 0; shift < 32; shift += 8)
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

std::optional<std::string> wideToUtf8(const std::wstring &value)
{
    if (value.empty())
    {
        return std::string{};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size,
            nullptr, nullptr) != size)
    {
        return std::nullopt;
    }
    return result;
}

std::optional<std::wstring> utf8ToWide(const std::string &value)
{
    if (value.empty())
    {
        return std::wstring{};
    }
    const int size = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0)
    {
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size) != size)
    {
        return std::nullopt;
    }
    return result;
}

bool appendString(std::vector<std::uint8_t> &bytes, const std::string &value)
{
    if (value.size() > MaximumStringBytes)
    {
        return false;
    }
    put32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
    return true;
}

bool takeString(
    const std::vector<std::uint8_t> &bytes,
    std::size_t &offset,
    std::string &value)
{
    if (bytes.size() - offset < 4)
    {
        return false;
    }
    const std::uint32_t size = get32(bytes.data() + offset);
    offset += 4;
    if (size > MaximumStringBytes || bytes.size() - offset < size)
    {
        return false;
    }
    value.assign(
        reinterpret_cast<const char *>(bytes.data() + offset),
        size);
    offset += size;
    return true;
}

} // namespace

std::vector<std::uint8_t> encodeStartRequest(const StartRequest &request)
{
    const auto executable = wideToUtf8(request.sshExecutable.wstring());
    const auto working = wideToUtf8(request.workingDirectory.wstring());
    const auto askPass = wideToUtf8(request.askPassHelper.wstring());
    if (!executable || !working || !askPass)
    {
        return {};
    }
    std::vector<std::uint8_t> bytes;
    put16(bytes, StartVersion);
    put16(bytes, request.ssh.port);
    bytes.push_back(request.ssh.authentication
            == SshAuthenticationMode::StoredPassword
        ? 1U : 0U);
    bytes.push_back(request.ssh.username ? 1U : 0U);
    put16(bytes, 0);
    if (!appendString(bytes, request.ssh.host)
        || !appendString(bytes, request.ssh.username.value_or(""))
        || !appendString(bytes, *executable)
        || !appendString(bytes, *working)
        || !appendString(bytes, *askPass))
    {
        return {};
    }
    return bytes;
}

bool decodeStartRequest(
    const std::vector<std::uint8_t> &payload,
    StartRequest &request,
    std::string &error)
{
    if (payload.size() < 8 || get16(payload.data()) != StartVersion)
    {
        error = "broker start version is invalid";
        return false;
    }
    request.ssh.port = get16(payload.data() + 2);
    const std::uint8_t authentication = payload[4];
    const std::uint8_t hasUsername = payload[5];
    if (authentication > 1 || hasUsername > 1)
    {
        error = "broker start flags are invalid";
        return false;
    }
    request.ssh.authentication = authentication == 1
        ? SshAuthenticationMode::StoredPassword
        : SshAuthenticationMode::SystemDefault;
    std::size_t offset = 8;
    std::string username;
    std::string executable;
    std::string working;
    std::string askPass;
    if (!takeString(payload, offset, request.ssh.host)
        || !takeString(payload, offset, username)
        || !takeString(payload, offset, executable)
        || !takeString(payload, offset, working)
        || !takeString(payload, offset, askPass)
        || offset != payload.size())
    {
        error = "broker start payload is truncated or oversized";
        return false;
    }
    if (hasUsername)
    {
        request.ssh.username = username;
    }
    else if (!username.empty())
    {
        error = "broker start username flag is inconsistent";
        return false;
    }
    const auto executableWide = utf8ToWide(executable);
    const auto workingWide = utf8ToWide(working);
    const auto askPassWide = utf8ToWide(askPass);
    if (!executableWide || !workingWide || !askPassWide)
    {
        error = "broker start path is not valid UTF-8";
        return false;
    }
    request.sshExecutable = *executableWide;
    request.workingDirectory = *workingWide;
    request.askPassHelper = *askPassWide;
    return true;
}

} // namespace dirbridge::terminal::broker
