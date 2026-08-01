#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <sddl.h>

#include "terminal/AskPassProtocol.h"
#include "terminal/StoredPasswordChannel.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace dirbridge::terminal {
namespace {

using namespace askpass;

class Handle
{
public:
    Handle() = default;
    explicit Handle(HANDLE value) : value_(value) {}
    Handle(const Handle &) = delete;
    Handle &operator=(const Handle &) = delete;
    Handle(Handle &&other) noexcept : value_(other.release()) {}
    Handle &operator=(Handle &&other) noexcept
    {
        if (this != &other)
        {
            reset(other.release());
        }
        return *this;
    }
    ~Handle() { reset(); }

    HANDLE get() const noexcept { return value_; }
    HANDLE release() noexcept
    {
        const HANDLE value = value_;
        value_ = INVALID_HANDLE_VALUE;
        return value;
    }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) noexcept
    {
        if (value_ != nullptr && value_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(value_);
        }
        value_ = value;
    }

private:
    HANDLE value_ = INVALID_HANDLE_VALUE;
};

std::wstring randomHex(std::size_t byteCount)
{
    std::vector<unsigned char> bytes(byteCount);
    if (BCryptGenRandom(
            nullptr,
            bytes.data(),
            static_cast<ULONG>(bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG)
        != 0)
    {
        return {};
    }
    constexpr wchar_t Hex[] = L"0123456789abcdef";
    std::wstring result;
    result.reserve(bytes.size() * 2);
    for (const unsigned char value : bytes)
    {
        result.push_back(Hex[value >> 4U]);
        result.push_back(Hex[value & 0x0FU]);
    }
    SecureZeroMemory(bytes.data(), bytes.size());
    return result;
}

std::string narrowHex(const std::wstring &value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value)
    {
        result.push_back(static_cast<char>(character));
    }
    return result;
}

std::optional<std::filesystem::path> normalizedExistingFile(
    const std::filesystem::path &path)
{
    std::error_code error;
    if (!path.is_absolute() || !std::filesystem::is_regular_file(path, error)
        || error)
    {
        return std::nullopt;
    }
    const auto normalized = std::filesystem::weakly_canonical(path, error);
    if (error)
    {
        return std::nullopt;
    }
    return normalized;
}

bool samePath(
    const std::filesystem::path &left,
    const std::filesystem::path &right)
{
    return CompareStringOrdinal(
               left.c_str(),
               -1,
               right.c_str(),
               -1,
               TRUE)
        == CSTR_EQUAL;
}

std::optional<std::filesystem::path> clientProcessPath(HANDLE pipe)
{
    ULONG clientPid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &clientPid) || clientPid == 0)
    {
        return std::nullopt;
    }
    Handle process(OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE,
        clientPid));
    if (process.get() == INVALID_HANDLE_VALUE || process.get() == nullptr)
    {
        return std::nullopt;
    }
    std::vector<wchar_t> path(32768);
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.get(), 0, path.data(), &size)
        || size == 0)
    {
        return std::nullopt;
    }
    return normalizedExistingFile(std::filesystem::path(
        std::wstring(path.data(), size)));
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
        DWORD written = 0;
        const DWORD part = static_cast<DWORD>(std::min<std::size_t>(
            size,
            64U * 1024U));
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
        DWORD received = 0;
        const DWORD part = static_cast<DWORD>(std::min<std::size_t>(
            size,
            64U * 1024U));
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

bool sendRejected(HANDLE pipe)
{
    std::vector<std::uint8_t> response;
    put32(response, Magic);
    put16(response, Version);
    put16(response, static_cast<std::uint16_t>(ResponseStatus::Rejected));
    put32(response, 0);
    return writeAll(pipe, response.data(), response.size());
}

} // namespace

struct StoredPasswordChannel::Impl
{
    StoredPasswordEndpoint endpoint;
    std::filesystem::path expectedHelperPath;
    StoredPasswordLease password;
    Handle pipe;
    bool served = false;

    Impl(
        StoredPasswordEndpoint endpointValue,
        std::filesystem::path helperPath,
        StoredPasswordLease passwordValue,
        Handle pipeValue)
        : endpoint(std::move(endpointValue))
        , expectedHelperPath(std::move(helperPath))
        , password(std::move(passwordValue))
        , pipe(std::move(pipeValue))
    {
    }
};

std::unique_ptr<StoredPasswordChannel> StoredPasswordChannel::create(
    StoredPasswordLease password,
    std::filesystem::path expectedHelperPath,
    std::string &error)
{
    const auto normalizedHelper = normalizedExistingFile(expectedHelperPath);
    if (!normalizedHelper)
    {
        error = "AskPass helper must be an existing absolute file";
        return nullptr;
    }

    StoredPasswordEndpoint endpoint;
    const std::wstring pipeSuffix = randomHex(16);
    endpoint.token = randomHex(32);
    if (pipeSuffix.empty() || endpoint.token.empty())
    {
        error = "Failed to generate AskPass channel identifiers";
        return nullptr;
    }
    endpoint.pipeName = L"\\\\.\\pipe\\DirBridge.SshAskPass." + pipeSuffix;

    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;;GA;;;OW)",
            SDDL_REVISION_1,
            &descriptor,
            nullptr))
    {
        error = "Failed to create AskPass pipe security descriptor";
        return nullptr;
    }
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.lpSecurityDescriptor = descriptor;
    security.bInheritHandle = FALSE;
    Handle pipe(CreateNamedPipeW(
        endpoint.pipeName.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT
            | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        4096,
        4096,
        10000,
        &security));
    LocalFree(descriptor);
    if (pipe.get() == INVALID_HANDLE_VALUE)
    {
        error = "Failed to create one-shot AskPass pipe";
        return nullptr;
    }

    auto impl = std::make_unique<Impl>(
        std::move(endpoint),
        *normalizedHelper,
        std::move(password),
        std::move(pipe));
    return std::unique_ptr<StoredPasswordChannel>(
        new StoredPasswordChannel(std::move(impl)));
}

StoredPasswordChannel::StoredPasswordChannel(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl))
{
}

StoredPasswordChannel::~StoredPasswordChannel() = default;

const StoredPasswordEndpoint &StoredPasswordChannel::endpoint() const noexcept
{
    return impl_->endpoint;
}

bool StoredPasswordChannel::serveOnce(std::string &error)
{
    if (impl_->served)
    {
        error = "AskPass channel has already been consumed";
        return false;
    }
    impl_->served = true;

    const bool connected = ConnectNamedPipe(impl_->pipe.get(), nullptr) != FALSE
        || GetLastError() == ERROR_PIPE_CONNECTED;
    if (!connected)
    {
        error = "AskPass helper did not connect";
        return false;
    }

    const auto clientPath = clientProcessPath(impl_->pipe.get());
    if (!clientPath || !samePath(*clientPath, impl_->expectedHelperPath))
    {
        sendRejected(impl_->pipe.get());
        error = "AskPass client executable was not the expected helper";
        return false;
    }

    std::array<std::uint8_t, HeaderSize> header{};
    if (!readAll(impl_->pipe.get(), header.data(), header.size()))
    {
        error = "AskPass request header was incomplete";
        return false;
    }
    const std::uint32_t tokenSize = get32(header.data() + 8);
    if (get32(header.data()) != Magic
        || get16(header.data() + 4) != Version
        || get16(header.data() + 6) != 0
        || tokenSize < MinTokenBytes
        || tokenSize > MaxTokenBytes)
    {
        sendRejected(impl_->pipe.get());
        error = "AskPass request header was invalid";
        return false;
    }
    std::string token(tokenSize, '\0');
    if (!readAll(impl_->pipe.get(), token.data(), token.size())
        || token != narrowHex(impl_->endpoint.token))
    {
        sendRejected(impl_->pipe.get());
        error = "AskPass request token was invalid";
        return false;
    }

    const bool sent = impl_->password.consume(
        [this](const char *bytes, std::size_t size) {
            std::vector<std::uint8_t> response;
            response.reserve(HeaderSize + size);
            put32(response, Magic);
            put16(response, Version);
            put16(response, static_cast<std::uint16_t>(
                ResponseStatus::Password));
            put32(response, static_cast<std::uint32_t>(size));
            response.insert(response.end(), bytes, bytes + size);
            const bool written = writeAll(
                impl_->pipe.get(),
                response.data(),
                response.size());
            if (!response.empty())
            {
                SecureZeroMemory(response.data(), response.size());
            }
            return written;
        });
    FlushFileBuffers(impl_->pipe.get());
    DisconnectNamedPipe(impl_->pipe.get());
    impl_->pipe.reset();
    if (!sent)
    {
        error = "AskPass password could not be sent";
    }
    return sent;
}

} // namespace dirbridge::terminal
