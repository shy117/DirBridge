#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ConPtyBrokerProtocol.h"
#include "ConPtyTestProtocol.h"

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using dirbridge::terminal::test::kOutputBlockCount;
using dirbridge::terminal::test::kOutputBlockSize;
using dirbridge::terminal::test::kOutputByteCount;
using dirbridge::terminal::test::kOutputTailMarker;
using dirbridge::terminal::test::makeOutputBlock;
using BrokerFrame = dirbridge::terminal::broker::test::Frame;
using BrokerFrameType = dirbridge::terminal::broker::test::FrameType;

std::atomic<int> gWorkerThreadCount{0};

class WorkerThreadScope
{
public:
    WorkerThreadScope()
    {
        ++gWorkerThreadCount;
    }

    ~WorkerThreadScope()
    {
        --gWorkerThreadCount;
    }
};

class UniqueHandle
{
public:
    UniqueHandle() = default;

    explicit UniqueHandle(HANDLE handle)
        : handle_(handle)
    {
    }

    ~UniqueHandle()
    {
        reset();
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    UniqueHandle(UniqueHandle&& other) noexcept
        : handle_(other.release())
    {
    }

    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    HANDLE get() const
    {
        return handle_;
    }

    explicit operator bool() const
    {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }

    HANDLE release()
    {
        const HANDLE released = handle_;
        handle_ = nullptr;
        return released;
    }

    void reset(HANDLE replacement = nullptr)
    {
        if (*this) {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

private:
    HANDLE handle_ = nullptr;
};

std::string windowsError(std::string_view operation);

struct BrokerMessage
{
    BrokerFrame frame;
    std::vector<std::uint8_t> payload;
};

void appendUint16(
    std::vector<std::uint8_t>& bytes,
    std::uint16_t value)
{
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8U));
}

void appendUint32(
    std::vector<std::uint8_t>& bytes,
    std::uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
    }
}

std::uint16_t readUint16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>(bytes[0])
        | static_cast<std::uint16_t>(bytes[1]) << 8U;
}

std::uint32_t readUint32(const std::uint8_t* bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | static_cast<std::uint32_t>(bytes[1]) << 8U
        | static_cast<std::uint32_t>(bytes[2]) << 16U
        | static_cast<std::uint32_t>(bytes[3]) << 24U;
}

void writeUint16(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint16_t value)
{
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8U);
}

void writeUint32(
    std::vector<std::uint8_t>& bytes,
    std::size_t offset,
    std::uint32_t value)
{
    for (unsigned int shift = 0; shift < 32; shift += 8) {
        bytes[offset + shift / 8U] =
            static_cast<std::uint8_t>(value >> shift);
    }
}

bool isKnownBrokerFrameType(BrokerFrameType type)
{
    switch (type) {
    case BrokerFrameType::Start:
    case BrokerFrameType::Ready:
    case BrokerFrameType::Stopped:
    case BrokerFrameType::Error:
        return true;
    }
    return false;
}

std::vector<std::uint8_t> encodeBrokerMessage(
    const BrokerFrame& frame,
    const std::vector<std::uint8_t>& payload)
{
    std::vector<std::uint8_t> bytes;
    bytes.reserve(
        dirbridge::terminal::broker::test::kFrameHeaderSize
        + payload.size());
    appendUint32(
        bytes,
        dirbridge::terminal::broker::test::kFrameMagic);
    appendUint16(
        bytes,
        dirbridge::terminal::broker::test::kProtocolVersion);
    appendUint16(
        bytes,
        dirbridge::terminal::broker::test::kFrameHeaderSize);
    appendUint32(bytes, static_cast<std::uint32_t>(frame.type));
    appendUint32(bytes, frame.generation);
    appendUint32(bytes, frame.sequence);
    appendUint32(bytes, static_cast<std::uint32_t>(payload.size()));
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    return bytes;
}

bool writeAll(
    HANDLE handle,
    const std::uint8_t* bytes,
    std::size_t size,
    std::string& error)
{
    std::size_t offset = 0;
    while (offset < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(
            size - offset,
            std::numeric_limits<DWORD>::max()));
        DWORD written = 0;
        if (!WriteFile(
                handle,
                bytes + offset,
                chunk,
                &written,
                nullptr)) {
            error = windowsError("WriteFile(broker frame)");
            return false;
        }
        if (written == 0) {
            error = "WriteFile(broker frame) wrote zero bytes";
            return false;
        }
        offset += written;
    }
    return true;
}

bool writeAll(
    HANDLE handle,
    const std::vector<std::uint8_t>& bytes,
    std::string& error)
{
    return writeAll(
        handle,
        bytes.data(),
        bytes.size(),
        error);
}

bool readToEnd(
    HANDLE handle,
    std::vector<std::uint8_t>& bytes,
    std::string& error)
{
    std::array<std::uint8_t, 4096> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(
                handle,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) {
                return true;
            }
            error = windowsError("ReadFile(broker frame)");
            return false;
        }
        if (read == 0) {
            return true;
        }
        if (bytes.size()
            > dirbridge::terminal::broker::test::kMaximumPayloadSize
                + dirbridge::terminal::broker::test::kFrameHeaderSize * 4U
                - read) {
            error = "broker frame stream exceeded bounded size";
            return false;
        }
        bytes.insert(bytes.end(), buffer.begin(), buffer.begin() + read);
    }
}

bool decodeBrokerMessages(
    const std::vector<std::uint8_t>& bytes,
    std::vector<BrokerMessage>& messages,
    std::string& error)
{
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        constexpr std::size_t kHeaderSize =
            dirbridge::terminal::broker::test::kFrameHeaderSize;
        if (bytes.size() - offset < kHeaderSize) {
            error = "truncated broker frame header";
            return false;
        }
        const std::uint8_t* header = bytes.data() + offset;
        if (readUint32(header) != dirbridge::terminal::broker::test::kFrameMagic
            || readUint16(header + 4)
                != dirbridge::terminal::broker::test::kProtocolVersion
            || readUint16(header + 6) != kHeaderSize) {
            error = "invalid broker frame preamble";
            return false;
        }
        const std::uint32_t payloadSize = readUint32(header + 20);
        if (payloadSize
            > dirbridge::terminal::broker::test::kMaximumPayloadSize) {
            error = "broker frame payload exceeded limit";
            return false;
        }
        if (bytes.size() - offset - kHeaderSize < payloadSize) {
            error = "truncated broker frame payload";
            return false;
        }

        BrokerMessage message;
        message.frame.type = static_cast<BrokerFrameType>(
            readUint32(header + 8));
        if (!isKnownBrokerFrameType(message.frame.type)) {
            error = "unknown broker frame type";
            return false;
        }
        message.frame.generation = readUint32(header + 12);
        message.frame.sequence = readUint32(header + 16);
        const auto payloadBegin = bytes.begin()
            + static_cast<std::ptrdiff_t>(offset + kHeaderSize);
        message.payload.assign(
            payloadBegin,
            payloadBegin + static_cast<std::ptrdiff_t>(payloadSize));
        messages.push_back(std::move(message));
        offset += kHeaderSize + payloadSize;
    }
    return true;
}

std::optional<std::string> toUtf8(const std::wstring& value)
{
    if (value.empty()) {
        return std::string{};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return std::nullopt;
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            size,
            nullptr,
            nullptr)
        != size) {
        return std::nullopt;
    }
    return result;
}

std::optional<std::wstring> fromUtf8(
    const std::vector<std::uint8_t>& value)
{
    if (value.empty()) {
        return std::wstring{};
    }
    const char* data = reinterpret_cast<const char*>(value.data());
    const int size = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        data,
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (size <= 0) {
        return std::nullopt;
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            data,
            static_cast<int>(value.size()),
            result.data(),
            size)
        != size) {
        return std::nullopt;
    }
    return result;
}

struct ConPtyApi
{
    using CreatePseudoConsoleFunction =
        HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HANDLE*);
    using ResizePseudoConsoleFunction = HRESULT(WINAPI*)(HANDLE, COORD);
    using ReleasePseudoConsoleFunction = HRESULT(WINAPI*)(HANDLE);
    using ClosePseudoConsoleFunction = void(WINAPI*)(HANDLE);

    CreatePseudoConsoleFunction create = nullptr;
    ResizePseudoConsoleFunction resize = nullptr;
    ReleasePseudoConsoleFunction release = nullptr;
    ClosePseudoConsoleFunction close = nullptr;
    std::string error;

    bool load()
    {
        HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
        if (!kernel32) {
            error = "GetModuleHandleW(kernel32.dll) failed";
            return false;
        }

        create = reinterpret_cast<CreatePseudoConsoleFunction>(
            GetProcAddress(kernel32, "CreatePseudoConsole"));
        resize = reinterpret_cast<ResizePseudoConsoleFunction>(
            GetProcAddress(kernel32, "ResizePseudoConsole"));
        release = reinterpret_cast<ReleasePseudoConsoleFunction>(
            GetProcAddress(kernel32, "ReleasePseudoConsole"));
        close = reinterpret_cast<ClosePseudoConsoleFunction>(
            GetProcAddress(kernel32, "ClosePseudoConsole"));
        if (!create || !resize || !close) {
            error = "ConPTY API is unavailable";
            return false;
        }
        return true;
    }
};

std::string windowsError(std::string_view operation)
{
    std::ostringstream stream;
    stream << operation << " failed with Win32 error " << GetLastError();
    return stream.str();
}

std::wstring quoteArgument(const std::wstring& value)
{
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : value) {
        if (character == L'\\') {
            ++backslashes;
            continue;
        }
        if (character == L'"') {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'"');
    return quoted;
}

DWORD currentHandleCount()
{
    DWORD count = 0;
    return GetProcessHandleCount(GetCurrentProcess(), &count) ? count : 0;
}

DWORD waitForSettledHandleCount(
    DWORD maximum,
    std::chrono::milliseconds timeout)
{
    const auto deadline = Clock::now() + timeout;
    DWORD observed = currentHandleCount();
    while (observed > maximum && Clock::now() < deadline) {
        std::this_thread::sleep_for(25ms);
        observed = currentHandleCount();
    }
    return observed;
}

struct NativeUnicodeString
{
    USHORT length = 0;
    USHORT maximumLength = 0;
    PWSTR buffer = nullptr;
};

struct NativeSystemHandleEntry
{
    PVOID object = nullptr;
    ULONG_PTR processId = 0;
    ULONG_PTR handleValue = 0;
    ULONG grantedAccess = 0;
    USHORT creatorBackTraceIndex = 0;
    USHORT objectTypeIndex = 0;
    ULONG handleAttributes = 0;
    ULONG reserved = 0;
};

struct NativeSystemHandleInformation
{
    ULONG_PTR handleCount = 0;
    ULONG_PTR reserved = 0;
    NativeSystemHandleEntry handles[1];
};

struct HandleRecord
{
    ULONG_PTR value = 0;
    ULONG grantedAccess = 0;
    ULONG attributes = 0;
    USHORT typeIndex = 0;
    std::string typeName;
    DWORD targetProcessId = 0;
    DWORD processExitCode = 0;
    std::string processImage;
};

struct HandleSnapshot
{
    DWORD reportedCount = 0;
    std::vector<HandleRecord> records;
    std::string error;
};

class NativeHandleInspector
{
public:
    bool load()
    {
        HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
        if (!ntdll) {
            error_ = windowsError("GetModuleHandleW(ntdll.dll)");
            return false;
        }

        querySystemInformation_ =
            reinterpret_cast<NtQuerySystemInformationFunction>(
                GetProcAddress(ntdll, "NtQuerySystemInformation"));
        queryObject_ = reinterpret_cast<NtQueryObjectFunction>(
            GetProcAddress(ntdll, "NtQueryObject"));
        if (!querySystemInformation_ || !queryObject_) {
            error_ = "native handle inspection APIs are unavailable";
            return false;
        }
        return true;
    }

    const std::string& error() const
    {
        return error_;
    }

    HandleSnapshot snapshot()
    {
        HandleSnapshot result;
        result.reportedCount = currentHandleCount();
        if (!querySystemInformation_ || !queryObject_) {
            result.error = "native handle inspector was not loaded";
            return result;
        }

        std::vector<std::uint8_t> buffer(256 * 1024);
        ULONG returnLength = 0;
        LONG status = 0;
        for (int attempt = 0; attempt < 8; ++attempt) {
            status = querySystemInformation_(
                kSystemExtendedHandleInformation,
                buffer.data(),
                static_cast<ULONG>(buffer.size()),
                &returnLength);
            if (status >= 0) {
                break;
            }
            if (!isBufferSizeStatus(status)) {
                result.error = nativeStatusError(
                    "NtQuerySystemInformation",
                    status);
                return result;
            }

            const std::size_t requested = returnLength > buffer.size()
                ? static_cast<std::size_t>(returnLength)
                : buffer.size() * 2;
            if (requested > kMaximumSnapshotBytes) {
                result.error = "system handle snapshot exceeded size limit";
                return result;
            }
            buffer.resize(requested + 64 * 1024);
        }
        if (status < 0) {
            result.error = "system handle snapshot did not stabilize";
            return result;
        }

        const auto* information =
            reinterpret_cast<const NativeSystemHandleInformation*>(
                buffer.data());
        const std::size_t headerSize =
            offsetof(NativeSystemHandleInformation, handles);
        const std::size_t availableEntries =
            (buffer.size() - headerSize) / sizeof(NativeSystemHandleEntry);
        const std::size_t entryCount = std::min<std::size_t>(
            static_cast<std::size_t>(information->handleCount),
            availableEntries);
        const ULONG_PTR currentProcessId = GetCurrentProcessId();
        for (std::size_t index = 0; index < entryCount; ++index) {
            const NativeSystemHandleEntry& entry =
                information->handles[index];
            if (entry.processId != currentProcessId) {
                continue;
            }

            HandleRecord record;
            record.value = entry.handleValue;
            record.grantedAccess = entry.grantedAccess;
            record.attributes = entry.handleAttributes;
            record.typeIndex = entry.objectTypeIndex;
            record.typeName = typeName(entry);
            populateProcessDetails(entry, record);
            result.records.push_back(std::move(record));
        }
        std::sort(
            result.records.begin(),
            result.records.end(),
            [](const HandleRecord& left, const HandleRecord& right) {
                return left.value < right.value;
            });
        return result;
    }

private:
    using NtQuerySystemInformationFunction =
        LONG(NTAPI*)(ULONG, PVOID, ULONG, PULONG);
    using NtQueryObjectFunction =
        LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

    static constexpr ULONG kSystemExtendedHandleInformation = 64;
    static constexpr ULONG kObjectTypeInformation = 2;
    static constexpr ULONG kStatusInfoLengthMismatch = 0xC0000004UL;
    static constexpr ULONG kStatusBufferTooSmall = 0xC0000023UL;
    static constexpr std::size_t kMaximumSnapshotBytes =
        128ULL * 1024ULL * 1024ULL;

    static bool isBufferSizeStatus(LONG status)
    {
        const ULONG value = static_cast<ULONG>(status);
        return value == kStatusInfoLengthMismatch
            || value == kStatusBufferTooSmall;
    }

    static std::string nativeStatusError(
        std::string_view operation,
        LONG status)
    {
        std::ostringstream stream;
        stream << operation << " failed with NTSTATUS 0x"
               << std::hex << static_cast<ULONG>(status);
        return stream.str();
    }

    static std::string narrowTypeName(const NativeUnicodeString& name)
    {
        if (!name.buffer || name.length == 0) {
            return {};
        }
        const int wideLength = name.length / sizeof(wchar_t);
        const int utf8Length = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            name.buffer,
            wideLength,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (utf8Length <= 0) {
            return {};
        }
        std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
        if (WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                name.buffer,
                wideLength,
                utf8.data(),
                utf8Length,
                nullptr,
                nullptr)
            != utf8Length) {
            return {};
        }
        return utf8;
    }

    static std::string narrowText(const wchar_t* text, int length)
    {
        if (!text || length <= 0) {
            return {};
        }
        const int utf8Length = WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text,
            length,
            nullptr,
            0,
            nullptr,
            nullptr);
        if (utf8Length <= 0) {
            return {};
        }
        std::string utf8(static_cast<std::size_t>(utf8Length), '\0');
        if (WideCharToMultiByte(
                CP_UTF8,
                WC_ERR_INVALID_CHARS,
                text,
                length,
                utf8.data(),
                utf8Length,
                nullptr,
                nullptr)
            != utf8Length) {
            return {};
        }
        return utf8;
    }

    static void populateProcessDetails(
        const NativeSystemHandleEntry& entry,
        HandleRecord& record)
    {
        if (record.typeName != "Process") {
            return;
        }

        const HANDLE process =
            reinterpret_cast<HANDLE>(entry.handleValue);
        record.targetProcessId = GetProcessId(process);
        GetExitCodeProcess(process, &record.processExitCode);

        std::vector<wchar_t> path(32768);
        DWORD pathLength = static_cast<DWORD>(path.size());
        if (QueryFullProcessImageNameW(
                process,
                0,
                path.data(),
                &pathLength)) {
            record.processImage = narrowText(
                path.data(),
                static_cast<int>(pathLength));
        }
    }

    std::string typeName(const NativeSystemHandleEntry& entry)
    {
        const auto cached = typeNames_.find(entry.objectTypeIndex);
        if (cached != typeNames_.end()) {
            return cached->second;
        }

        std::vector<std::uint8_t> buffer(1024);
        ULONG returnLength = 0;
        LONG status = queryObject_(
            reinterpret_cast<HANDLE>(entry.handleValue),
            kObjectTypeInformation,
            buffer.data(),
            static_cast<ULONG>(buffer.size()),
            &returnLength);
        if (isBufferSizeStatus(status) && returnLength > buffer.size()) {
            buffer.resize(returnLength);
            status = queryObject_(
                reinterpret_cast<HANDLE>(entry.handleValue),
                kObjectTypeInformation,
                buffer.data(),
                static_cast<ULONG>(buffer.size()),
                &returnLength);
        }

        std::string name;
        if (status >= 0) {
            name = narrowTypeName(
                *reinterpret_cast<const NativeUnicodeString*>(
                    buffer.data()));
        }
        if (name.empty()) {
            name = "type#" + std::to_string(entry.objectTypeIndex);
        }
        typeNames_.emplace(entry.objectTypeIndex, name);
        return name;
    }

    NtQuerySystemInformationFunction querySystemInformation_ = nullptr;
    NtQueryObjectFunction queryObject_ = nullptr;
    std::map<USHORT, std::string> typeNames_;
    std::string error_;
};

enum class CloseMode
{
    Natural,
    Cooperative,
    Forced
};

struct CloseResult
{
    bool completed = false;
    bool escalated = false;
    bool jobReachedZero = false;
    bool outputReachedEof = false;
    bool processExited = false;
    bool threadsJoined = false;
    bool handlesReleased = false;
    DWORD processId = 0;
    DWORD activeProcesses = std::numeric_limits<DWORD>::max();
    long long elapsedMilliseconds = 0;
    std::string error;
};

class ConPtySession
{
public:
    explicit ConPtySession(const ConPtyApi& api)
        : api_(api)
    {
    }

    ~ConPtySession()
    {
        emergencyCleanup();
    }

    ConPtySession(const ConPtySession&) = delete;
    ConPtySession& operator=(const ConPtySession&) = delete;

    bool start(
        const std::wstring& childPath,
        std::wstring_view mode,
        COORD initialSize = COORD{80, 25})
    {
        if (started_) {
            error_ = "session was already started";
            return false;
        }
        started_ = true;

        HANDLE pseudoInputRead = nullptr;
        HANDLE parentInputWrite = nullptr;
        if (!CreatePipe(
                &pseudoInputRead,
                &parentInputWrite,
                nullptr,
                0)) {
            return failStart(windowsError("CreatePipe(input)"));
        }
        UniqueHandle pseudoInputReadHandle(pseudoInputRead);
        inputWrite_.reset(parentInputWrite);

        HANDLE parentOutputRead = nullptr;
        HANDLE pseudoOutputWrite = nullptr;
        if (!CreatePipe(
                &parentOutputRead,
                &pseudoOutputWrite,
                nullptr,
                0)) {
            return failStart(windowsError("CreatePipe(output)"));
        }
        outputRead_.reset(parentOutputRead);
        UniqueHandle pseudoOutputWriteHandle(pseudoOutputWrite);

        HRESULT result = api_.create(
            initialSize,
            pseudoInputReadHandle.get(),
            pseudoOutputWriteHandle.get(),
            0,
            &pseudoConsole_);
        if (FAILED(result)) {
            std::ostringstream stream;
            stream << "CreatePseudoConsole failed with HRESULT 0x"
                   << std::hex << static_cast<unsigned long>(result);
            return failStart(stream.str());
        }

        job_.reset(CreateJobObjectW(nullptr, nullptr));
        if (!job_) {
            return failStart(windowsError("CreateJobObjectW"));
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!SetInformationJobObject(
                job_.get(),
                JobObjectExtendedLimitInformation,
                &limit,
                sizeof(limit))) {
            return failStart(windowsError(
                "SetInformationJobObject(limit)"));
        }

        completionPort_.reset(CreateIoCompletionPort(
            INVALID_HANDLE_VALUE,
            nullptr,
            0,
            1));
        if (!completionPort_) {
            return failStart(windowsError("CreateIoCompletionPort"));
        }

        JOBOBJECT_ASSOCIATE_COMPLETION_PORT association{};
        association.CompletionKey = this;
        association.CompletionPort = completionPort_.get();
        if (!SetInformationJobObject(
                job_.get(),
                JobObjectAssociateCompletionPortInformation,
                &association,
                sizeof(association))) {
            return failStart(windowsError(
                "SetInformationJobObject(completion port)"));
        }

        SIZE_T attributeListSize = 0;
        InitializeProcThreadAttributeList(
            nullptr,
            1,
            0,
            &attributeListSize);
        std::vector<std::uint8_t> attributeStorage(attributeListSize);
        auto* attributeList =
            reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
                attributeStorage.data());
        if (!InitializeProcThreadAttributeList(
                attributeList,
                1,
                0,
                &attributeListSize)) {
            return failStart(windowsError(
                "InitializeProcThreadAttributeList"));
        }

        struct AttributeListGuard
        {
            LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
            ~AttributeListGuard()
            {
                if (value) {
                    DeleteProcThreadAttributeList(value);
                }
            }
        } attributeGuard{attributeList};

        if (!UpdateProcThreadAttribute(
                attributeList,
                0,
                PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                pseudoConsole_,
                sizeof(pseudoConsole_),
                nullptr,
                nullptr)) {
            return failStart(windowsError(
                "UpdateProcThreadAttribute(PSEUDOCONSOLE)"));
        }

        std::wstring commandLine =
            quoteArgument(childPath)
            + L" --mode "
            + std::wstring(mode);
        std::vector<wchar_t> mutableCommandLine(
            commandLine.begin(),
            commandLine.end());
        mutableCommandLine.push_back(L'\0');

        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributeList;
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(
                childPath.c_str(),
                mutableCommandLine.data(),
                nullptr,
                nullptr,
                FALSE,
                EXTENDED_STARTUPINFO_PRESENT
                    | CREATE_SUSPENDED
                    | CREATE_UNICODE_ENVIRONMENT,
                nullptr,
                nullptr,
                &startup.StartupInfo,
                &process)) {
            return failStart(windowsError("CreateProcessW"));
        }
        process_.reset(process.hProcess);
        primaryThread_.reset(process.hThread);
        processId_ = process.dwProcessId;

        if (!AssignProcessToJobObject(job_.get(), process_.get())) {
            return failStart(windowsError("AssignProcessToJobObject"));
        }

        pseudoInputReadHandle.reset();
        pseudoOutputWriteHandle.reset();

        try {
            outputThread_ = std::thread([this] { runOutputThread(); });
            inputThread_ = std::thread([this] { runInputThread(); });
            closeThread_ = std::thread([this] { runCloseThread(); });
            jobThread_ = std::thread([this] { runJobThread(); });
        } catch (const std::exception& exception) {
            return failStart(
                std::string("worker thread creation failed: ")
                + exception.what());
        }

        if (ResumeThread(primaryThread_.get()) == static_cast<DWORD>(-1)) {
            return failStart(windowsError("ResumeThread"));
        }
        primaryThread_.reset();
        running_ = true;
        return true;
    }

    const std::string& error() const
    {
        return error_;
    }

    DWORD processId() const
    {
        return processId_;
    }

    bool send(std::string data)
    {
        std::lock_guard<std::mutex> lock(inputMutex_);
        if (inputStop_ || !inputWrite_) {
            return false;
        }
        inputQueue_.push_back(std::move(data));
        inputCondition_.notify_one();
        return true;
    }

    bool resize(COORD size)
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (!pseudoConsole_ || closeRequested_) {
            error_ = "ResizePseudoConsole called after close started";
            return false;
        }
        const HRESULT result = api_.resize(pseudoConsole_, size);
        if (FAILED(result)) {
            std::ostringstream stream;
            stream << "ResizePseudoConsole failed with HRESULT 0x"
                   << std::hex << static_cast<unsigned long>(result);
            error_ = stream.str();
            return false;
        }
        return true;
    }

    bool waitForOutput(
        std::string_view needle,
        std::chrono::milliseconds timeout)
    {
        const auto matches = [](std::uint8_t outputByte, char needleByte) {
            return outputByte
                == static_cast<std::uint8_t>(
                    static_cast<unsigned char>(needleByte));
        };
        const auto contains = [this, needle] {
            return std::search(
                       output_.begin(),
                       output_.end(),
                       needle.begin(),
                       needle.end(),
                       [](std::uint8_t outputByte, char needleByte) {
                           return outputByte
                               == static_cast<std::uint8_t>(
                                   static_cast<unsigned char>(needleByte));
                       })
                    != output_.end()
                || outputEof_;
        };

        std::unique_lock<std::mutex> lock(outputMutex_);
        if (!outputCondition_.wait_for(lock, timeout, contains)) {
            return false;
        }
        return std::search(
                   output_.begin(),
                   output_.end(),
                   needle.begin(),
                   needle.end(),
                   matches)
            != output_.end();
    }

    bool waitForOutputSize(
        std::size_t size,
        std::chrono::milliseconds timeout)
    {
        std::unique_lock<std::mutex> lock(outputMutex_);
        outputCondition_.wait_for(lock, timeout, [this, size] {
            return output_.size() >= size || outputEof_;
        });
        return output_.size() >= size;
    }

    std::vector<std::uint8_t> takeOutput()
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        return std::move(output_);
    }

    std::string diagnosticState()
    {
        std::scoped_lock lock(outputMutex_, stateMutex_);
        std::ostringstream stream;
        stream << "output_bytes=" << output_.size()
               << " output_eof=" << outputEof_
               << " output_error=" << outputReadError_
               << " job_zero=" << jobZero_
               << " input_error=" << inputWriteFailed_;
        return stream.str();
    }

    std::string outputPreview()
    {
        std::lock_guard<std::mutex> lock(outputMutex_);
        const std::size_t previewSize = std::min<std::size_t>(
            output_.size(),
            256);
        std::ostringstream stream;
        stream << std::hex << std::setfill('0');
        for (std::size_t index = 0; index < previewSize; ++index) {
            stream << std::setw(2)
                   << static_cast<unsigned>(output_[index]);
        }
        return stream.str();
    }

    CloseResult finish(
        CloseMode mode,
        bool closeBeforeWaiting = false)
    {
        CloseResult result;
        result.processId = processId_;
        const auto startedAt = Clock::now();
        const auto escalationDeadline = startedAt + 2s;
        const auto completionDeadline = startedAt + 5s;

        if (!running_) {
            result.error = "finish called for a session that is not running";
            return result;
        }

        if (mode == CloseMode::Cooperative && !send("x")) {
            result.error = "failed to queue cooperative close input";
            emergencyCleanup();
            return result;
        }

        if (closeBeforeWaiting) {
            requestPseudoConsoleClose();
            if (mode == CloseMode::Forced) {
                if (!TerminateJobObject(job_.get(), 0xE001)) {
                    result.error = windowsError("TerminateJobObject");
                    emergencyCleanup();
                    return result;
                }
                result.escalated = true;
            }
        }

        if (!waitForJobZero(escalationDeadline)) {
            if (!TerminateJobObject(job_.get(), 0xE001)) {
                result.error = windowsError("TerminateJobObject");
                emergencyCleanup();
                return result;
            }
            result.escalated = true;
        }

        if (!waitForJobZero(completionDeadline)) {
            result.error =
                "Job did not reach zero active processes within 5 seconds";
            emergencyCleanup();
            return result;
        }
        result.jobReachedZero = true;
        result.activeProcesses = queryActiveProcessCount();

        stopAndJoinInputThread();
        if (!closeBeforeWaiting) {
            requestPseudoConsoleClose();
        }

        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            stateCondition_.wait_until(lock, completionDeadline, [this] {
                return closeCompleted_;
            });
        }
        {
            std::unique_lock<std::mutex> lock(outputMutex_);
            outputCondition_.wait_until(lock, completionDeadline, [this] {
                return outputEof_;
            });
        }

        joinThread(closeThread_);
        joinThread(outputThread_);
        stopAndJoinJobThread();

        result.outputReachedEof = outputEof_;
        result.processExited =
            process_
            && WaitForSingleObject(process_.get(), 0) == WAIT_OBJECT_0;

        inputWrite_.reset();
        outputRead_.reset();
        primaryThread_.reset();
        process_.reset();
        job_.reset();
        completionPort_.reset();
        running_ = false;

        result.threadsJoined =
            !inputThread_.joinable()
            && !outputThread_.joinable()
            && !closeThread_.joinable()
            && !jobThread_.joinable()
            && gWorkerThreadCount.load() == 0;
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            result.handlesReleased =
                !inputWrite_
                && !outputRead_
                && !primaryThread_
                && !process_
                && !job_
                && !completionPort_
                && pseudoConsole_ == nullptr;
        }
        result.elapsedMilliseconds =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                Clock::now() - startedAt)
                .count();
        result.completed =
            result.jobReachedZero
            && result.activeProcesses == 0
            && result.outputReachedEof
            && result.processExited
            && result.threadsJoined
            && result.handlesReleased
            && !releasePseudoConsoleFailed_
            && result.elapsedMilliseconds <= 5000;
        if (!result.completed && result.error.empty()) {
            result.error = releasePseudoConsoleFailed_
                ? "ReleasePseudoConsole failed"
                : "lifecycle completion invariant failed";
        }
        return result;
    }

private:
    bool failStart(std::string message)
    {
        error_ = std::move(message);
        emergencyCleanup();
        return false;
    }

    void runInputThread()
    {
        WorkerThreadScope workerScope;
        for (;;) {
            std::string data;
            {
                std::unique_lock<std::mutex> lock(inputMutex_);
                inputCondition_.wait(lock, [this] {
                    return inputStop_ || !inputQueue_.empty();
                });
                if (inputQueue_.empty()) {
                    if (inputStop_) {
                        return;
                    }
                    continue;
                }
                data = std::move(inputQueue_.front());
                inputQueue_.pop_front();
            }

            const char* cursor = data.data();
            std::size_t remaining = data.size();
            while (remaining > 0) {
                DWORD written = 0;
                const DWORD chunk = remaining > MAXDWORD
                    ? MAXDWORD
                    : static_cast<DWORD>(remaining);
                if (!WriteFile(
                        inputWrite_.get(),
                        cursor,
                        chunk,
                        &written,
                        nullptr)
                    || written == 0) {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    inputWriteFailed_ = true;
                    stateCondition_.notify_all();
                    return;
                }
                cursor += written;
                remaining -= written;
            }
        }
    }

    void runOutputThread()
    {
        WorkerThreadScope workerScope;
        std::array<std::uint8_t, 64 * 1024> buffer{};
        for (;;) {
            DWORD read = 0;
            if (ReadFile(
                    outputRead_.get(),
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr)
                && read > 0) {
                {
                    std::lock_guard<std::mutex> lock(outputMutex_);
                    output_.insert(
                        output_.end(),
                        buffer.begin(),
                        buffer.begin() + read);
                }
                outputCondition_.notify_all();
                continue;
            }

            const DWORD error = GetLastError();
            {
                std::lock_guard<std::mutex> lock(outputMutex_);
                outputEof_ =
                    read == 0
                    || error == ERROR_BROKEN_PIPE
                    || error == ERROR_OPERATION_ABORTED;
                if (!outputEof_) {
                    outputReadError_ = error;
                }
            }
            outputCondition_.notify_all();
            stateCondition_.notify_all();
            return;
        }
    }

    void runCloseThread()
    {
        WorkerThreadScope workerScope;
        HANDLE pseudoConsole = nullptr;
        {
            std::unique_lock<std::mutex> lock(stateMutex_);
            stateCondition_.wait(lock, [this] {
                return closeRequested_;
            });
            pseudoConsole = pseudoConsole_;
        }

        if (pseudoConsole) {
            if (api_.release) {
                if (FAILED(api_.release(pseudoConsole))) {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    releasePseudoConsoleFailed_ = true;
                } else {
                    std::unique_lock<std::mutex> lock(outputMutex_);
                    outputCondition_.wait_for(lock, 5s, [this] {
                        return outputEof_;
                    });
                }
            }
            api_.close(pseudoConsole);
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            pseudoConsole_ = nullptr;
            closeCompleted_ = true;
        }
        stateCondition_.notify_all();
    }

    void runJobThread()
    {
        WorkerThreadScope workerScope;
        for (;;) {
            DWORD message = 0;
            ULONG_PTR completionKey = 0;
            LPOVERLAPPED overlapped = nullptr;
            const BOOL received = GetQueuedCompletionStatus(
                completionPort_.get(),
                &message,
                &completionKey,
                &overlapped,
                INFINITE);
            if (completionKey == 0) {
                return;
            }
            if (completionKey != reinterpret_cast<ULONG_PTR>(this)) {
                continue;
            }
            if (!received && !overlapped) {
                continue;
            }
            if (message == JOB_OBJECT_MSG_NEW_PROCESS) {
                std::lock_guard<std::mutex> lock(stateMutex_);
                sawJobProcess_ = true;
            }
            if (message == JOB_OBJECT_MSG_ACTIVE_PROCESS_ZERO) {
                {
                    std::lock_guard<std::mutex> lock(stateMutex_);
                    jobZero_ = true;
                }
                stateCondition_.notify_all();
                return;
            }
        }
    }

    bool waitForJobZero(Clock::time_point deadline)
    {
        std::unique_lock<std::mutex> lock(stateMutex_);
        return stateCondition_.wait_until(lock, deadline, [this] {
            return jobZero_;
        });
    }

    DWORD queryActiveProcessCount() const
    {
        if (!job_) {
            return std::numeric_limits<DWORD>::max();
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        if (!QueryInformationJobObject(
                job_.get(),
                JobObjectBasicAccountingInformation,
                &accounting,
                sizeof(accounting),
                nullptr)) {
            return std::numeric_limits<DWORD>::max();
        }
        return accounting.ActiveProcesses;
    }

    void requestPseudoConsoleClose()
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            closeRequested_ = true;
        }
        stateCondition_.notify_all();
    }

    static void joinThread(std::thread& thread)
    {
        if (thread.joinable()) {
            thread.join();
        }
    }

    void stopAndJoinInputThread()
    {
        {
            std::lock_guard<std::mutex> lock(inputMutex_);
            inputStop_ = true;
        }
        inputCondition_.notify_all();
        if (inputThread_.joinable()) {
            inputThread_.join();
        }
    }

    void stopAndJoinJobThread()
    {
        if (jobThread_.joinable()) {
            PostQueuedCompletionStatus(
                completionPort_.get(),
                0,
                0,
                nullptr);
            jobThread_.join();
        }
    }

    void emergencyCleanup()
    {
        if (cleanupStarted_) {
            return;
        }
        cleanupStarted_ = true;

        if (job_) {
            TerminateJobObject(job_.get(), 0xE002);
        } else if (process_) {
            TerminateProcess(process_.get(), 0xE002);
        }
        if (process_
            && WaitForSingleObject(process_.get(), 0) != WAIT_OBJECT_0) {
            TerminateProcess(process_.get(), 0xE002);
        }
        if (process_) {
            WaitForSingleObject(process_.get(), 1000);
        }

        stopAndJoinInputThread();

        if (closeThread_.joinable()) {
            requestPseudoConsoleClose();
            closeThread_.join();
        } else if (pseudoConsole_) {
            HANDLE pseudoConsole = pseudoConsole_;
            std::thread fallbackClose([this, pseudoConsole] {
                WorkerThreadScope workerScope;
                if (api_.release) {
                    api_.release(pseudoConsole);
                }
                api_.close(pseudoConsole);
            });
            fallbackClose.join();
            pseudoConsole_ = nullptr;
            closeCompleted_ = true;
        }

        if (outputThread_.joinable()) {
            outputThread_.join();
        }
        stopAndJoinJobThread();

        inputWrite_.reset();
        outputRead_.reset();
        primaryThread_.reset();
        process_.reset();
        job_.reset();
        completionPort_.reset();
        running_ = false;
    }

    const ConPtyApi& api_;
    bool started_ = false;
    bool running_ = false;
    bool cleanupStarted_ = false;
    std::string error_;
    DWORD processId_ = 0;

    HANDLE pseudoConsole_ = nullptr;
    UniqueHandle inputWrite_;
    UniqueHandle outputRead_;
    UniqueHandle process_;
    UniqueHandle primaryThread_;
    UniqueHandle job_;
    UniqueHandle completionPort_;

    std::thread inputThread_;
    std::thread outputThread_;
    std::thread closeThread_;
    std::thread jobThread_;

    std::mutex inputMutex_;
    std::condition_variable inputCondition_;
    std::deque<std::string> inputQueue_;
    bool inputStop_ = false;

    std::mutex outputMutex_;
    std::condition_variable outputCondition_;
    std::vector<std::uint8_t> output_;
    bool outputEof_ = false;
    DWORD outputReadError_ = ERROR_SUCCESS;

    std::mutex stateMutex_;
    std::condition_variable stateCondition_;
    bool closeRequested_ = false;
    bool closeCompleted_ = false;
    bool jobZero_ = false;
    bool sawJobProcess_ = false;
    bool inputWriteFailed_ = false;
    bool releasePseudoConsoleFailed_ = false;
};

class Reporter
{
public:
    bool check(bool condition, std::string_view name)
    {
        std::cout << (condition ? "[PASS] " : "[FAIL] ")
                  << name << std::endl;
        if (!condition) {
            ++failureCount_;
        }
        return condition;
    }

    void metric(std::string_view name, long long value)
    {
        std::cout << "[METRIC] " << name << '=' << value << std::endl;
    }

    void metric(std::string_view name, std::string_view value)
    {
        std::cout << "[METRIC] " << name << '=' << value << std::endl;
    }

    int failureCount() const
    {
        return failureCount_;
    }

private:
    int failureCount_ = 0;
};

std::map<std::string, long long> handleTypeCounts(
    const HandleSnapshot& snapshot)
{
    std::map<std::string, long long> counts;
    for (const HandleRecord& record : snapshot.records) {
        ++counts[record.typeName];
    }
    return counts;
}

bool containsHandleValue(
    const HandleSnapshot& snapshot,
    ULONG_PTR value)
{
    const auto found = std::lower_bound(
        snapshot.records.begin(),
        snapshot.records.end(),
        value,
        [](const HandleRecord& record, ULONG_PTR candidate) {
            return record.value < candidate;
        });
    return found != snapshot.records.end() && found->value == value;
}

void printHandleSnapshot(
    std::string_view checkpoint,
    const HandleSnapshot& baseline,
    const HandleSnapshot& previous,
    const HandleSnapshot& current,
    DWORD expectedProcessId = 0)
{
    std::cout << "[HANDLE] checkpoint=" << checkpoint
              << " reported=" << current.reportedCount
              << " enumerated=" << current.records.size()
              << " delta="
              << static_cast<long long>(current.reportedCount)
                    - static_cast<long long>(baseline.reportedCount)
              << std::endl;

    const auto baselineCounts = handleTypeCounts(baseline);
    const auto currentCounts = handleTypeCounts(current);
    std::map<std::string, std::pair<long long, long long>> changes;
    for (const auto& [type, count] : baselineCounts) {
        changes[type].first = count;
    }
    for (const auto& [type, count] : currentCounts) {
        changes[type].second = count;
    }
    for (const auto& [type, counts] : changes) {
        const long long delta = counts.second - counts.first;
        if (checkpoint != "baseline" && delta == 0) {
            continue;
        }
        std::cout << "[HANDLE-TYPE] checkpoint=" << checkpoint
                  << " type=\"" << type << "\""
                  << " count=" << counts.second
                  << " delta=" << delta << std::endl;
    }

    if (checkpoint == "baseline") {
        return;
    }
    for (const HandleRecord& record : current.records) {
        if (containsHandleValue(previous, record.value)) {
            continue;
        }
        std::cout << "[HANDLE-NEW] checkpoint=" << checkpoint
                  << " value=0x" << std::hex << record.value
                  << " type=\"" << record.typeName << "\""
                  << " access=0x" << record.grantedAccess
                  << " attributes=0x" << record.attributes
                  << std::dec;
        if (record.targetProcessId != 0) {
            std::cout << " target_pid=" << record.targetProcessId
                      << " exit_code=" << record.processExitCode;
            if (expectedProcessId != 0) {
                std::cout << " matches_child="
                          << (record.targetProcessId
                                  == expectedProcessId
                              ? 1
                              : 0);
            }
        }
        if (!record.processImage.empty()) {
            std::cout << " image=\"" << record.processImage << '"';
        }
        std::cout << std::endl;
    }
}

void printActiveProcessHandles(
    std::string_view checkpoint,
    const HandleSnapshot& previous,
    const HandleSnapshot& active,
    DWORD childProcessId)
{
    for (const HandleRecord& record : active.records) {
        if (record.typeName != "Process"
            || containsHandleValue(previous, record.value)) {
            continue;
        }
        std::cout << "[HANDLE-ACTIVE] checkpoint=" << checkpoint
                  << " value=0x" << std::hex << record.value
                  << " access=0x" << record.grantedAccess
                  << std::dec
                  << " target_pid=" << record.targetProcessId
                  << " matches_child="
                  << (record.targetProcessId == childProcessId ? 1 : 0);
        if (!record.processImage.empty()) {
            std::cout << " image=\"" << record.processImage << '"';
        }
        std::cout << std::endl;
    }
}

bool verifyCloseResult(
    Reporter& reporter,
    const CloseResult& result,
    std::string_view prefix,
    bool expectEscalation,
    long long maximumMilliseconds)
{
    bool passed = true;
    passed &= reporter.check(result.completed, std::string(prefix) + " completed");
    passed &= reporter.check(
        result.escalated == expectEscalation,
        std::string(prefix) + " escalation state");
    passed &= reporter.check(
        result.elapsedMilliseconds <= maximumMilliseconds,
        std::string(prefix) + " time budget");
    passed &= reporter.check(
        result.jobReachedZero && result.activeProcesses == 0,
        std::string(prefix) + " Job active process zero");
    passed &= reporter.check(
        result.outputReachedEof,
        std::string(prefix) + " output EOF");
    passed &= reporter.check(
        result.threadsJoined,
        std::string(prefix) + " worker joins");
    passed &= reporter.check(
        result.handlesReleased,
        std::string(prefix) + " native handles released");
    if (!result.error.empty()) {
        std::cout << "[DETAIL] " << prefix << ": " << result.error
                  << std::endl;
    }
    return passed;
}

bool startReadySession(
    Reporter& reporter,
    ConPtySession& session,
    const std::wstring& child,
    std::wstring_view mode,
    std::string_view ready,
    COORD size = COORD{80, 25})
{
    if (!reporter.check(
            session.start(child, mode, size),
            "start child session")) {
        std::cout << "[DETAIL] " << session.error() << std::endl;
        return false;
    }
    if (!reporter.check(
            session.waitForOutput(ready, 2s),
            "child readiness marker")) {
        std::cout << "[DETAIL] " << session.diagnosticState()
                  << std::endl;
        return false;
    }
    return true;
}

bool runSmokeSuite(
    Reporter& reporter,
    const ConPtyApi& api,
    const std::wstring& child)
{
    reporter.check(true, "dynamic ConPTY API availability");

    {
        ConPtySession session(api);
        if (!startReadySession(
                reporter,
                session,
                child,
                L"utf8-echo",
                "READY ECHO")) {
            return false;
        }
        const std::string expected = u8"DirBridge UTF-8 回显 ✓";
        session.send(expected);
        if (!reporter.check(
                session.waitForOutput(expected, 2s),
                "UTF-8 round trip")) {
            std::cout << "[DETAIL] " << session.diagnosticState()
                      << " output_hex=" << session.outputPreview()
                      << std::endl;
            return false;
        }
        session.send("__DIRBRIDGE_ECHO_END__\r\n");
        const CloseResult result = session.finish(CloseMode::Natural);
        if (!verifyCloseResult(
                reporter,
                result,
                "UTF-8 session",
                false,
                2000)) {
            return false;
        }
    }

    {
        ConPtySession session(api);
        if (!startReadySession(
                reporter,
                session,
                child,
                L"size-report",
                "READY SIZE")) {
            return false;
        }
        if (!reporter.check(
                session.resize(COORD{120, 40}),
                "single ConPTY resize")) {
            return false;
        }
        session.send("s");
        if (!reporter.check(
                session.waitForOutput("SIZE 120 40", 1s),
                "resized dimensions observed")) {
            return false;
        }
        session.send("q");
        const CloseResult result = session.finish(CloseMode::Natural);
        if (!verifyCloseResult(
                reporter,
                result,
                "resize session",
                false,
                2000)) {
            return false;
        }
    }

    {
        ConPtySession session(api);
        if (!startReadySession(
                reporter,
                session,
                child,
                L"cooperative-close",
                "READY COOPERATIVE")) {
            return false;
        }
        const CloseResult result = session.finish(CloseMode::Cooperative);
        reporter.metric(
            "smoke.cooperative_close_ms",
            result.elapsedMilliseconds);
        if (!verifyCloseResult(
                reporter,
                result,
                "cooperative close",
                false,
                2000)) {
            return false;
        }
    }

    {
        ConPtySession session(api);
        if (!startReadySession(
                reporter,
                session,
                child,
                L"stubborn-close",
                "READY STUBBORN")) {
            return false;
        }
        const CloseResult result = session.finish(CloseMode::Forced);
        reporter.metric(
            "smoke.forced_close_ms",
            result.elapsedMilliseconds);
        if (!verifyCloseResult(
                reporter,
                result,
                "forced close",
                true,
                5000)) {
            return false;
        }
    }

    {
        ConPtySession session(api);
        if (!startReadySession(
                reporter,
                session,
                child,
                L"spawn-resident",
                "SPAWNED ")) {
            return false;
        }
        const CloseResult result = session.finish(CloseMode::Forced);
        if (!verifyCloseResult(
                reporter,
                result,
                "descendant tree close",
                true,
                5000)) {
            return false;
        }
    }

    {
        std::filesystem::path missing =
            std::filesystem::path(child).parent_path()
            / L"DirBridgeConPtyMissingChild.exe";
        {
            ConPtySession warmUp(api);
            if (!reporter.check(
                    !warmUp.start(
                        missing.wstring(),
                        L"cooperative-close"),
                    "failure path warm-up")) {
                return false;
            }
        }
        const DWORD baselineHandles = currentHandleCount();
        {
            ConPtySession session(api);
            reporter.check(
                !session.start(
                    missing.wstring(),
                    L"cooperative-close"),
                "CreateProcessW failure is reported");
        }
        const DWORD finalHandles = waitForSettledHandleCount(
            baselineHandles + 4,
            5s);
        reporter.metric(
            "smoke.failure_path_handle_delta",
            static_cast<long long>(finalHandles)
                - static_cast<long long>(baselineHandles));
        if (!reporter.check(
                gWorkerThreadCount.load() == 0,
                "failure path worker rollback")) {
            return false;
        }
        if (!reporter.check(
                finalHandles <= baselineHandles + 4,
                "failure path handle rollback")) {
            return false;
        }
    }

    return reporter.failureCount() == 0;
}

class Sha256
{
public:
    Sha256()
    {
        if (BCryptOpenAlgorithmProvider(
                &algorithm_,
                BCRYPT_SHA256_ALGORITHM,
                nullptr,
                0)
            < 0) {
            return;
        }

        DWORD resultSize = 0;
        if (BCryptGetProperty(
                algorithm_,
                BCRYPT_OBJECT_LENGTH,
                reinterpret_cast<PUCHAR>(&objectSize_),
                sizeof(objectSize_),
                &resultSize,
                0)
            < 0) {
            return;
        }
        object_.resize(objectSize_);
        if (BCryptCreateHash(
                algorithm_,
                &hash_,
                object_.data(),
                static_cast<ULONG>(object_.size()),
                nullptr,
                0,
                0)
            < 0) {
            return;
        }
        valid_ = true;
    }

    ~Sha256()
    {
        if (hash_) {
            BCryptDestroyHash(hash_);
        }
        if (algorithm_) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
        }
    }

    bool update(const std::uint8_t* data, std::size_t size)
    {
        if (!valid_) {
            return false;
        }
        while (size > 0) {
            const ULONG chunk = size > std::numeric_limits<ULONG>::max()
                ? std::numeric_limits<ULONG>::max()
                : static_cast<ULONG>(size);
            if (BCryptHashData(
                    hash_,
                    const_cast<PUCHAR>(data),
                    chunk,
                    0)
                < 0) {
                valid_ = false;
                return false;
            }
            data += chunk;
            size -= chunk;
        }
        return true;
    }

    std::optional<std::array<std::uint8_t, 32>> finish()
    {
        if (!valid_) {
            return std::nullopt;
        }
        std::array<std::uint8_t, 32> digest{};
        if (BCryptFinishHash(
                hash_,
                digest.data(),
                static_cast<ULONG>(digest.size()),
                0)
            < 0) {
            valid_ = false;
            return std::nullopt;
        }
        valid_ = false;
        return digest;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    DWORD objectSize_ = 0;
    std::vector<std::uint8_t> object_;
    bool valid_ = false;
};

std::string toHex(const std::array<std::uint8_t, 32>& digest)
{
    std::ostringstream stream;
    stream << std::hex << std::setfill('0');
    for (const std::uint8_t byte : digest) {
        stream << std::setw(2) << static_cast<unsigned>(byte);
    }
    return stream.str();
}

std::vector<std::uint8_t> stripVtControlSequences(
    const std::vector<std::uint8_t>& input)
{
    enum class State
    {
        Ground,
        Escape,
        ControlSequence,
        StringSequence,
        StringEscape
    };

    State state = State::Ground;
    std::vector<std::uint8_t> text;
    text.reserve(input.size());
    for (const std::uint8_t byte : input) {
        switch (state) {
        case State::Ground:
            if (byte == 0x1b) {
                state = State::Escape;
            } else if (byte == '\r') {
                while (!text.empty() && text.back() == ' ') {
                    text.pop_back();
                }
                text.push_back(byte);
            } else if (
                byte == '\n'
                || (byte >= 0x20 && byte != 0x7f)) {
                text.push_back(byte);
            }
            break;
        case State::Escape:
            if (byte == '[') {
                state = State::ControlSequence;
            } else if (
                byte == ']'
                || byte == 'P'
                || byte == 'X'
                || byte == '^'
                || byte == '_') {
                state = State::StringSequence;
            } else {
                state = State::Ground;
            }
            break;
        case State::ControlSequence:
            if (byte >= 0x40 && byte <= 0x7e) {
                state = State::Ground;
            }
            break;
        case State::StringSequence:
            if (byte == 0x07) {
                state = State::Ground;
            } else if (byte == 0x1b) {
                state = State::StringEscape;
            }
            break;
        case State::StringEscape:
            state = byte == '\\'
                ? State::Ground
                : State::StringSequence;
            break;
        }
    }
    return text;
}

bool validate64MiBOutput(
    Reporter& reporter,
    const std::vector<std::uint8_t>& output)
{
    const auto terminalText = stripVtControlSequences(output);
    const auto firstBlock = makeOutputBlock(0);
    constexpr std::size_t kHeaderProbeSize = 32;
    const auto payloadBegin = std::search(
        terminalText.begin(),
        terminalText.end(),
        firstBlock.begin(),
        firstBlock.begin() + kHeaderProbeSize);
    if (!reporter.check(
            payloadBegin != terminalText.end(),
            "64 MiB payload start marker")) {
        return false;
    }

    const auto payloadOffset = static_cast<std::size_t>(
        std::distance(terminalText.begin(), payloadBegin));
    const auto availablePayloadBytes =
        terminalText.size() - payloadOffset;
    if (!reporter.check(
            availablePayloadBytes >= kOutputByteCount,
            "64 MiB exact byte count")) {
        reporter.metric(
            "full.available_payload_bytes",
            static_cast<long long>(availablePayloadBytes));
        return false;
    }
    reporter.metric(
        "full.raw_output_bytes",
        static_cast<long long>(output.size()));
    reporter.metric(
        "full.terminal_text_bytes",
        static_cast<long long>(terminalText.size()));
    reporter.metric(
        "full.payload_offset",
        static_cast<long long>(payloadOffset));
    reporter.metric(
        "full.payload_bytes",
        static_cast<long long>(kOutputByteCount));

    Sha256 actualHash;
    Sha256 expectedHash;
    actualHash.update(
        terminalText.data() + payloadOffset,
        kOutputByteCount);
    bool sequenceMatches = true;
    for (std::size_t sequence = 0;
         sequence < kOutputBlockCount;
         ++sequence) {
        const auto expected = makeOutputBlock(sequence);
        expectedHash.update(expected.data(), expected.size());
        const auto offset =
            payloadOffset + sequence * kOutputBlockSize;
        const auto mismatch = std::mismatch(
            expected.begin(),
            expected.end(),
            terminalText.begin() + offset);
        if (mismatch.first != expected.end()) {
            const auto blockOffset = static_cast<std::size_t>(
                std::distance(expected.begin(), mismatch.first));
            const auto absoluteOffset = offset + blockOffset;
            reporter.metric(
                "full.first_mismatch_sequence",
                static_cast<long long>(sequence));
            reporter.metric(
                "full.first_mismatch_block_offset",
                static_cast<long long>(blockOffset));
            std::ostringstream detail;
            detail << std::hex << std::setfill('0');
            const auto previewEnd = std::min<std::size_t>(
                terminalText.size(),
                absoluteOffset + 32);
            for (std::size_t index = absoluteOffset;
                 index < previewEnd;
                 ++index) {
                detail << std::setw(2)
                       << static_cast<unsigned>(terminalText[index]);
            }
            reporter.metric(
                "full.first_mismatch_actual_hex",
                detail.str());
            sequenceMatches = false;
            break;
        }
    }
    if (!reporter.check(
            sequenceMatches,
            "64 MiB continuous sequence")) {
        return false;
    }

    const bool tailMatches = std::equal(
        kOutputTailMarker.begin(),
        kOutputTailMarker.end(),
        terminalText.begin()
            + payloadOffset
            + kOutputByteCount
            - 2
            - kOutputTailMarker.size());
    if (!reporter.check(tailMatches, "64 MiB tail marker")) {
        return false;
    }

    const auto actualDigest = actualHash.finish();
    const auto expectedDigest = expectedHash.finish();
    if (!reporter.check(
            actualDigest.has_value() && expectedDigest.has_value(),
            "SHA-256 calculation")) {
        return false;
    }
    reporter.metric("full.output_sha256", toHex(*actualDigest));
    return reporter.check(
        *actualDigest == *expectedDigest,
        "64 MiB SHA-256");
}

bool runCooperativeIteration(
    const ConPtyApi& api,
    const std::wstring& child,
    CloseResult& result,
    bool closeBeforeWaiting = false)
{
    ConPtySession session(api);
    if (!session.start(child, L"cooperative-close")
        || !session.waitForOutput("READY COOPERATIVE", 2s)) {
        return false;
    }
    result = session.finish(
        CloseMode::Cooperative,
        closeBeforeWaiting);
    return result.completed
        && !result.escalated
        && result.elapsedMilliseconds <= 2000
        && result.activeProcesses == 0;
}

bool runForcedIteration(
    const ConPtyApi& api,
    const std::wstring& child,
    CloseResult& result,
    bool closeBeforeWaiting = false)
{
    ConPtySession session(api);
    if (!session.start(child, L"stubborn-close")
        || !session.waitForOutput("READY STUBBORN", 2s)) {
        return false;
    }
    result = session.finish(
        CloseMode::Forced,
        closeBeforeWaiting);
    return result.completed
        && result.escalated
        && result.elapsedMilliseconds <= 5000
        && result.activeProcesses == 0;
}

bool runHandleDiagnosticSuite(
    Reporter& reporter,
    const ConPtyApi& api,
    const std::wstring& child,
    bool closeBeforeWaiting)
{
    constexpr int kIterationsPerMode = 5;
    NativeHandleInspector inspector;
    if (!reporter.check(
            inspector.load(),
            "native handle inspector availability")) {
        std::cout << "[DETAIL] " << inspector.error() << std::endl;
        return false;
    }

    HandleSnapshot inspectorWarmUp = inspector.snapshot();
    if (!reporter.check(
            inspectorWarmUp.error.empty(),
            "native handle inspector warm-up")) {
        std::cout << "[DETAIL] " << inspectorWarmUp.error << std::endl;
        return false;
    }

    CloseResult warmUpResult;
    if (!reporter.check(
            runCooperativeIteration(
                api,
                child,
                warmUpResult,
                closeBeforeWaiting),
            "diagnostic cooperative warm-up")) {
        std::cout << "[DETAIL] " << warmUpResult.error << std::endl;
        return false;
    }
    if (!reporter.check(
            runForcedIteration(
                api,
                child,
                warmUpResult,
                closeBeforeWaiting),
            "diagnostic forced warm-up")) {
        std::cout << "[DETAIL] " << warmUpResult.error << std::endl;
        return false;
    }

    std::this_thread::sleep_for(100ms);
    const HandleSnapshot baseline = inspector.snapshot();
    if (!reporter.check(
            baseline.error.empty(),
            "capture diagnostic handle baseline")) {
        std::cout << "[DETAIL] " << baseline.error << std::endl;
        return false;
    }
    printHandleSnapshot("baseline", baseline, baseline, baseline);
    reporter.metric(
        "diagnostic.release_pseudo_console_available",
        api.release ? 1 : 0);
    reporter.metric(
        "diagnostic.iterations_per_mode",
        kIterationsPerMode);
    reporter.metric(
        "diagnostic.close_before_waiting",
        closeBeforeWaiting ? 1 : 0);

    HandleSnapshot previous = baseline;
    std::vector<DWORD> handleSamples;
    auto runIterations = [&] (
                             std::string_view name,
                             bool forced) {
        for (int iteration = 0;
             iteration < kIterationsPerMode;
             ++iteration) {
            ConPtySession session(api);
            const std::wstring_view mode = forced
                ? L"stubborn-close"
                : L"cooperative-close";
            const std::string_view ready = forced
                ? "READY STUBBORN"
                : "READY COOPERATIVE";
            const bool started = session.start(child, mode)
                && session.waitForOutput(ready, 2s);
            if (!started) {
                reporter.check(false, "start diagnostic session");
                std::cout << "[DETAIL] " << session.error()
                          << ' ' << session.diagnosticState()
                          << std::endl;
                return false;
            }

            const std::string checkpoint =
                std::string(name)
                + '-'
                + std::to_string(iteration + 1);
            const HandleSnapshot active = inspector.snapshot();
            if (!active.error.empty()) {
                reporter.check(
                    false,
                    "capture active diagnostic handle sample");
                std::cout << "[DETAIL] " << active.error << std::endl;
                return false;
            }
            printActiveProcessHandles(
                checkpoint,
                previous,
                active,
                session.processId());

            CloseResult result = session.finish(
                forced ? CloseMode::Forced : CloseMode::Cooperative,
                closeBeforeWaiting);
            const bool completed = result.completed
                && result.escalated == forced
                && result.elapsedMilliseconds <= (forced ? 5000 : 2000)
                && result.activeProcesses == 0;
            if (!completed) {
                reporter.check(
                    false,
                    std::string("diagnostic ")
                        + std::string(name)
                        + " iteration");
                std::cout << "[DETAIL] " << result.error << std::endl;
                return false;
            }

            std::this_thread::sleep_for(50ms);
            HandleSnapshot current = inspector.snapshot();
            if (!current.error.empty()) {
                reporter.check(false, "capture diagnostic handle sample");
                std::cout << "[DETAIL] " << current.error << std::endl;
                return false;
            }

            std::cout << "[SESSION] checkpoint=" << checkpoint
                      << " child_pid=" << result.processId
                      << std::endl;
            printHandleSnapshot(
                checkpoint,
                baseline,
                previous,
                current,
                result.processId);
            handleSamples.push_back(current.reportedCount);
            previous = std::move(current);
        }
        reporter.check(
            true,
            std::string("five diagnostic ")
                + std::string(name)
                + " iterations");
        return true;
    };

    if (!runIterations("cooperative", false)
        || !runIterations("forced", true)) {
        return false;
    }

    const DWORD settledCount = waitForSettledHandleCount(
        baseline.reportedCount + 4,
        5s);
    const HandleSnapshot final = inspector.snapshot();
    if (!reporter.check(
            final.error.empty(),
            "capture final diagnostic handle sample")) {
        std::cout << "[DETAIL] " << final.error << std::endl;
        return false;
    }
    printHandleSnapshot("final", baseline, previous, final);

    std::size_t growthEvents = 0;
    std::size_t declineEvents = 0;
    for (std::size_t index = 1; index < handleSamples.size(); ++index) {
        if (handleSamples[index] > handleSamples[index - 1]) {
            ++growthEvents;
        } else if (handleSamples[index] < handleSamples[index - 1]) {
            ++declineEvents;
        }
    }
    const bool sustainedMonotonicGrowth =
        growthEvents >= 2 && declineEvents == 0;
    reporter.metric("diagnostic.handle_baseline", baseline.reportedCount);
    reporter.metric("diagnostic.handle_settled", settledCount);
    reporter.metric("diagnostic.handle_final", final.reportedCount);
    reporter.metric(
        "diagnostic.handle_delta",
        static_cast<long long>(final.reportedCount)
            - static_cast<long long>(baseline.reportedCount));
    reporter.metric(
        "diagnostic.handle_growth_events",
        static_cast<long long>(growthEvents));
    reporter.metric(
        "diagnostic.handle_decline_events",
        static_cast<long long>(declineEvents));

    if (!reporter.check(
            gWorkerThreadCount.load() == 0,
            "diagnostic own worker count returns to zero")) {
        return false;
    }
    if (!reporter.check(
            final.reportedCount <= baseline.reportedCount + 4,
            "diagnostic handle delta within four")) {
        return false;
    }
    if (!reporter.check(
            !sustainedMonotonicGrowth,
            "diagnostic handle samples have no monotonic growth")) {
        return false;
    }
    return reporter.failureCount() == 0;
}

std::optional<std::wstring> currentExecutablePath()
{
    std::vector<wchar_t> path(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr,
        path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return std::nullopt;
    }
    return std::wstring(path.data(), length);
}

bool runBrokerWorkerSuite(
    Reporter& reporter,
    const ConPtyApi& api,
    const std::wstring& child)
{
    CloseResult cooperativeResult;
    if (!reporter.check(
            runCooperativeIteration(
                api,
                child,
                cooperativeResult),
            "broker worker cooperative session")) {
        std::cout << "[DETAIL] " << cooperativeResult.error
                  << std::endl;
        return false;
    }

    CloseResult forcedResult;
    if (!reporter.check(
            runForcedIteration(api, child, forcedResult),
            "broker worker forced session")) {
        std::cout << "[DETAIL] " << forcedResult.error << std::endl;
        return false;
    }
    return reporter.failureCount() == 0;
}

bool runBrokerProcess(
    const std::wstring& checker,
    const std::wstring& child,
    std::string& error)
{
    std::wstring commandLine =
        quoteArgument(checker)
        + L" --child "
        + quoteArgument(child)
        + L" --suite broker-worker";
    std::vector<wchar_t> mutableCommandLine(
        commandLine.begin(),
        commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            checker.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        error = windowsError("CreateProcessW(broker worker)");
        return false;
    }

    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);
    threadHandle.reset();
    const DWORD waitResult = WaitForSingleObject(
        processHandle.get(),
        15'000);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateProcess(processHandle.get(), 0xE003);
        WaitForSingleObject(processHandle.get(), 1000);
        error = waitResult == WAIT_TIMEOUT
            ? "broker worker exceeded 15 second timeout"
            : windowsError("WaitForSingleObject(broker worker)");
        return false;
    }

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(processHandle.get(), &exitCode)) {
        error = windowsError("GetExitCodeProcess(broker worker)");
        return false;
    }
    if (exitCode != 0) {
        error = "broker worker exited with code "
            + std::to_string(exitCode);
        return false;
    }
    return true;
}

bool runBrokerDiagnosticSuite(
    Reporter& reporter,
    const std::wstring& child)
{
    constexpr int kBrokerIterations = 5;
    const auto checker = currentExecutablePath();
    if (!reporter.check(
            checker.has_value(),
            "resolve broker worker executable")) {
        return false;
    }

    std::string error;
    if (!reporter.check(
            runBrokerProcess(*checker, child, error),
            "broker worker warm-up")) {
        std::cout << "[DETAIL] " << error << std::endl;
        return false;
    }

    const DWORD baselineHandles = currentHandleCount();
    std::vector<DWORD> handleSamples;
    for (int iteration = 0;
         iteration < kBrokerIterations;
         ++iteration) {
        error.clear();
        if (!reporter.check(
                runBrokerProcess(*checker, child, error),
                "broker worker iteration")) {
            std::cout << "[DETAIL] " << error << std::endl;
            return false;
        }
        const DWORD handles = currentHandleCount();
        handleSamples.push_back(handles);
        std::cout << "[BROKER] iteration=" << (iteration + 1)
                  << " sessions=" << (iteration + 1) * 2
                  << " parent_handles=" << handles
                  << " delta="
                  << static_cast<long long>(handles)
                        - static_cast<long long>(baselineHandles)
                  << std::endl;
    }

    const DWORD finalHandles = waitForSettledHandleCount(
        baselineHandles + 4,
        5s);
    std::size_t growthEvents = 0;
    std::size_t declineEvents = 0;
    for (std::size_t index = 1; index < handleSamples.size(); ++index) {
        if (handleSamples[index] > handleSamples[index - 1]) {
            ++growthEvents;
        } else if (handleSamples[index] < handleSamples[index - 1]) {
            ++declineEvents;
        }
    }
    const bool sustainedMonotonicGrowth =
        growthEvents >= 2 && declineEvents == 0;
    reporter.metric("broker.iterations", kBrokerIterations);
    reporter.metric("broker.sessions", kBrokerIterations * 2);
    reporter.metric("broker.handle_baseline", baselineHandles);
    reporter.metric("broker.handle_final", finalHandles);
    reporter.metric(
        "broker.handle_delta",
        static_cast<long long>(finalHandles)
            - static_cast<long long>(baselineHandles));
    reporter.metric(
        "broker.handle_growth_events",
        static_cast<long long>(growthEvents));
    reporter.metric(
        "broker.handle_decline_events",
        static_cast<long long>(declineEvents));

    if (!reporter.check(
            finalHandles <= baselineHandles + 4,
            "broker parent handle delta within four")) {
        return false;
    }
    if (!reporter.check(
            !sustainedMonotonicGrowth,
            "broker parent handle samples have no monotonic growth")) {
        return false;
    }
    return reporter.failureCount() == 0;
}

bool validateBrokerResponseMessages(
    const std::vector<BrokerMessage>& messages,
    std::uint32_t generation,
    std::string& error)
{
    if (messages.size() != 2
        || messages[0].frame.type != BrokerFrameType::Ready
        || messages[1].frame.type != BrokerFrameType::Stopped
        || messages[0].frame.generation != generation
        || messages[1].frame.generation != generation
        || messages[0].frame.sequence != 1
        || messages[1].frame.sequence != 2
        || !messages[0].payload.empty()
        || !messages[1].payload.empty()) {
        error = "broker IPC response sequence was invalid";
        return false;
    }
    return true;
}

bool runBrokerProtocolValidation(Reporter& reporter)
{
    const std::vector<std::uint8_t> startPayload = {'a', 'b', 'c'};
    const auto start = encodeBrokerMessage(
        BrokerFrame{BrokerFrameType::Start, 17, 1},
        startPayload);
    const auto ready = encodeBrokerMessage(
        BrokerFrame{BrokerFrameType::Ready, 17, 1},
        {});
    const auto stopped = encodeBrokerMessage(
        BrokerFrame{BrokerFrameType::Stopped, 17, 2},
        {});

    std::vector<std::uint8_t> merged = ready;
    merged.insert(merged.end(), stopped.begin(), stopped.end());
    std::vector<BrokerMessage> messages;
    std::string error;
    reporter.check(
        decodeBrokerMessages(merged, messages, error)
            && validateBrokerResponseMessages(messages, 17, error),
        "decode merged ready/stopped frames");

    auto rejects = [&](std::vector<std::uint8_t> bytes,
                       const char* label) {
        messages.clear();
        error.clear();
        return reporter.check(
            !decodeBrokerMessages(bytes, messages, error)
                && !error.empty(),
            label);
    };

    auto badMagic = start;
    badMagic[0] ^= 0xFFU;
    rejects(std::move(badMagic), "reject bad broker frame magic");

    auto badVersion = start;
    writeUint16(
        badVersion,
        4,
        dirbridge::terminal::broker::test::kProtocolVersion + 1U);
    rejects(std::move(badVersion), "reject unknown broker protocol version");

    auto badHeaderSize = start;
    writeUint16(badHeaderSize, 6, 16);
    rejects(std::move(badHeaderSize), "reject invalid broker header size");

    auto unknownType = start;
    writeUint32(unknownType, 8, 0xFFFFU);
    rejects(std::move(unknownType), "reject unknown broker frame type");

    auto oversized = start;
    writeUint32(
        oversized,
        20,
        dirbridge::terminal::broker::test::kMaximumPayloadSize + 1U);
    rejects(std::move(oversized), "reject oversized broker payload");

    auto truncatedHeader = start;
    truncatedHeader.resize(
        dirbridge::terminal::broker::test::kFrameHeaderSize - 1U);
    rejects(std::move(truncatedHeader), "reject truncated broker header");

    auto truncatedPayload = start;
    truncatedPayload.pop_back();
    rejects(std::move(truncatedPayload), "reject truncated broker payload");

    reporter.check(
        !fromUtf8(std::vector<std::uint8_t>{0xC3U, 0x28U}),
        "reject invalid UTF-8 broker start payload");

    messages.clear();
    error.clear();
    if (decodeBrokerMessages(merged, messages, error)
        && messages.size() == 2) {
        messages[1].frame.sequence = 3;
        reporter.check(
            !validateBrokerResponseMessages(messages, 17, error),
            "reject invalid broker response sequence");
    } else {
        reporter.check(
            false,
            "prepare invalid broker response sequence");
    }

    return reporter.failureCount() == 0;
}

bool runBrokerIpcProcess(
    const std::wstring& broker,
    const std::wstring& child,
    std::uint32_t generation,
    std::string& error)
{
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE commandReadRaw = nullptr;
    HANDLE commandWriteRaw = nullptr;
    if (!CreatePipe(
            &commandReadRaw,
            &commandWriteRaw,
            &security,
            0)) {
        error = windowsError("CreatePipe(broker command)");
        return false;
    }
    UniqueHandle commandRead(commandReadRaw);
    UniqueHandle commandWrite(commandWriteRaw);

    HANDLE eventReadRaw = nullptr;
    HANDLE eventWriteRaw = nullptr;
    if (!CreatePipe(
            &eventReadRaw,
            &eventWriteRaw,
            &security,
            0)) {
        error = windowsError("CreatePipe(broker event)");
        return false;
    }
    UniqueHandle eventRead(eventReadRaw);
    UniqueHandle eventWrite(eventWriteRaw);

    if (!SetHandleInformation(
            commandWrite.get(),
            HANDLE_FLAG_INHERIT,
            0)
        || !SetHandleInformation(
            eventRead.get(),
            HANDLE_FLAG_INHERIT,
            0)) {
        error = windowsError("SetHandleInformation(broker pipe)");
        return false;
    }

    UniqueHandle brokerJob(CreateJobObjectW(nullptr, nullptr));
    if (!brokerJob) {
        error = windowsError("CreateJobObjectW(broker)");
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            brokerJob.get(),
            JobObjectExtendedLimitInformation,
            &limit,
            sizeof(limit))) {
        error = windowsError("SetInformationJobObject(broker)");
        return false;
    }

    SIZE_T attributeListSize = 0;
    InitializeProcThreadAttributeList(
        nullptr,
        1,
        0,
        &attributeListSize);
    std::vector<std::uint8_t> attributeStorage(attributeListSize);
    auto* attributeList =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            attributeStorage.data());
    if (!InitializeProcThreadAttributeList(
            attributeList,
            1,
            0,
            &attributeListSize)) {
        error = windowsError(
            "InitializeProcThreadAttributeList(broker)");
        return false;
    }
    struct AttributeListGuard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
        ~AttributeListGuard()
        {
            if (value) {
                DeleteProcThreadAttributeList(value);
            }
        }
    } attributeGuard{attributeList};

    HANDLE inheritedHandles[] = {
        commandRead.get(),
        eventWrite.get(),
    };
    if (!UpdateProcThreadAttribute(
            attributeList,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles,
            sizeof(inheritedHandles),
            nullptr,
            nullptr)) {
        error = windowsError(
            "UpdateProcThreadAttribute(HANDLE_LIST)");
        return false;
    }

    std::wstring commandLine = quoteArgument(broker)
        + L" --command-handle "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(
            commandRead.get()))
        + L" --event-handle "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(
            eventWrite.get()));
    std::vector<wchar_t> mutableCommandLine(
        commandLine.begin(),
        commandLine.end());
    mutableCommandLine.push_back(L'\0');

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributeList;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            broker.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_SUSPENDED
                | CREATE_UNICODE_ENVIRONMENT
                | EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &process)) {
        error = windowsError("CreateProcessW(broker)");
        return false;
    }
    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);

    if (!AssignProcessToJobObject(
            brokerJob.get(),
            processHandle.get())) {
        TerminateProcess(processHandle.get(), 0xE010);
        WaitForSingleObject(processHandle.get(), 1000);
        error = windowsError("AssignProcessToJobObject(broker)");
        return false;
    }
    if (ResumeThread(threadHandle.get()) == static_cast<DWORD>(-1)) {
        TerminateJobObject(brokerJob.get(), 0xE011);
        WaitForSingleObject(processHandle.get(), 1000);
        error = windowsError("ResumeThread(broker)");
        return false;
    }
    threadHandle.reset();
    commandRead.reset();
    eventWrite.reset();

    const auto utf8Child = toUtf8(child);
    if (!utf8Child) {
        TerminateJobObject(brokerJob.get(), 0xE012);
        WaitForSingleObject(processHandle.get(), 1000);
        error = "failed to encode child path as UTF-8";
        return false;
    }
    const std::vector<std::uint8_t> payload(
        utf8Child->begin(),
        utf8Child->end());
    const auto startFrame = encodeBrokerMessage(
        BrokerFrame{BrokerFrameType::Start, generation, 1},
        payload);
    constexpr std::size_t kFirstFragmentSize = 7;
    constexpr std::size_t kSecondFragmentSize = 9;
    const bool startWritten =
        writeAll(
            commandWrite.get(),
            startFrame.data(),
            kFirstFragmentSize,
            error)
        && writeAll(
            commandWrite.get(),
            startFrame.data() + kFirstFragmentSize,
            kSecondFragmentSize,
            error)
        && writeAll(
            commandWrite.get(),
            startFrame.data()
                + kFirstFragmentSize
                + kSecondFragmentSize,
            startFrame.size()
                - kFirstFragmentSize
                - kSecondFragmentSize,
            error);
    if (!startWritten) {
        TerminateJobObject(brokerJob.get(), 0xE013);
        WaitForSingleObject(processHandle.get(), 1000);
        return false;
    }
    commandWrite.reset();

    const DWORD waitResult = WaitForSingleObject(
        processHandle.get(),
        15'000);
    if (waitResult != WAIT_OBJECT_0) {
        TerminateJobObject(brokerJob.get(), 0xE014);
        WaitForSingleObject(processHandle.get(), 1000);
        error = waitResult == WAIT_TIMEOUT
            ? "broker IPC process exceeded 15 second timeout"
            : windowsError("WaitForSingleObject(broker IPC)");
        return false;
    }

    DWORD exitCode = 0;
    if (!GetExitCodeProcess(processHandle.get(), &exitCode)) {
        error = windowsError("GetExitCodeProcess(broker IPC)");
        return false;
    }

    std::vector<std::uint8_t> responseBytes;
    if (!readToEnd(eventRead.get(), responseBytes, error)) {
        return false;
    }
    std::vector<BrokerMessage> messages;
    if (!decodeBrokerMessages(responseBytes, messages, error)) {
        return false;
    }
    if (exitCode != 0) {
        if (!messages.empty()
            && messages.back().frame.type == BrokerFrameType::Error) {
            error = "broker reported: " + std::string(
                messages.back().payload.begin(),
                messages.back().payload.end());
        } else {
            error = "broker IPC process exited with code "
                + std::to_string(exitCode);
        }
        return false;
    }
    return validateBrokerResponseMessages(
        messages,
        generation,
        error);
}

bool runBrokerIpcDiagnosticSuite(
    Reporter& reporter,
    const std::wstring& broker,
    const std::wstring& child,
    int brokerIterations)
{
    if (!runBrokerProtocolValidation(reporter)) {
        return false;
    }
    std::string error;
    if (!reporter.check(
            runBrokerIpcProcess(broker, child, 1, error),
            "framed broker warm-up")) {
        std::cout << "[DETAIL] " << error << std::endl;
        return false;
    }

    const DWORD baselineHandles = currentHandleCount();
    std::vector<DWORD> handleSamples;
    for (int iteration = 0; iteration < brokerIterations; ++iteration) {
        error.clear();
        const std::uint32_t generation =
            static_cast<std::uint32_t>(iteration + 2);
        if (!reporter.check(
                runBrokerIpcProcess(
                    broker,
                    child,
                    generation,
                    error),
                "framed broker iteration")) {
            std::cout << "[DETAIL] " << error << std::endl;
            return false;
        }
        const DWORD handles = currentHandleCount();
        handleSamples.push_back(handles);
        std::cout << "[BROKER-IPC] iteration=" << (iteration + 1)
                  << " generation=" << generation
                  << " sessions=" << (iteration + 1) * 2
                  << " parent_handles=" << handles
                  << " delta="
                  << static_cast<long long>(handles)
                        - static_cast<long long>(baselineHandles)
                  << std::endl;
    }

    const DWORD finalHandles = waitForSettledHandleCount(
        baselineHandles + 4,
        5s);
    std::size_t growthEvents = 0;
    std::size_t declineEvents = 0;
    for (std::size_t index = 1; index < handleSamples.size(); ++index) {
        if (handleSamples[index] > handleSamples[index - 1]) {
            ++growthEvents;
        } else if (handleSamples[index] < handleSamples[index - 1]) {
            ++declineEvents;
        }
    }
    const bool sustainedMonotonicGrowth =
        growthEvents >= 2 && declineEvents == 0;
    reporter.metric("broker_ipc.iterations", brokerIterations);
    reporter.metric("broker_ipc.sessions", brokerIterations * 2);
    reporter.metric("broker_ipc.handle_baseline", baselineHandles);
    reporter.metric("broker_ipc.handle_final", finalHandles);
    reporter.metric(
        "broker_ipc.handle_delta",
        static_cast<long long>(finalHandles)
            - static_cast<long long>(baselineHandles));
    reporter.metric(
        "broker_ipc.handle_growth_events",
        static_cast<long long>(growthEvents));
    reporter.metric(
        "broker_ipc.handle_decline_events",
        static_cast<long long>(declineEvents));

    if (!reporter.check(
            finalHandles <= baselineHandles + 4,
            "framed broker parent handle delta within four")) {
        return false;
    }
    if (!reporter.check(
            !sustainedMonotonicGrowth,
            "framed broker parent handles have no monotonic growth")) {
        return false;
    }
    return reporter.failureCount() == 0;
}

bool run64MiBValidation(
    Reporter& reporter,
    const ConPtyApi& api,
    const std::wstring& child)
{
    ConPtySession session(api);
    if (!reporter.check(
            session.start(
                child,
                L"output-64m",
                COORD{132, 43}),
            "start 64 MiB output child")) {
        std::cout << "[DETAIL] " << session.error() << std::endl;
        return false;
    }
    if (!reporter.check(
            session.waitForOutput(kOutputTailMarker, 300s),
            "receive 64 MiB output")) {
        std::cout << "[DETAIL] " << session.diagnosticState()
                  << std::endl;
        return false;
    }
    const CloseResult result = session.finish(CloseMode::Natural);
    if (!verifyCloseResult(
            reporter,
            result,
            "64 MiB output session",
            false,
            2000)) {
        return false;
    }
    const auto output = session.takeOutput();
    return validate64MiBOutput(reporter, output);
}

bool runFullResizeValidation(
    Reporter& reporter,
    const ConPtyApi& api,
    const std::wstring& child)
{
    ConPtySession session(api);
    if (!startReadySession(
            reporter,
            session,
            child,
            L"size-report",
            "READY SIZE")) {
        return false;
    }
    COORD finalSize{132, 43};
    for (int iteration = 0; iteration < 200; ++iteration) {
        const COORD size{
            static_cast<SHORT>(80 + (iteration % 53)),
            static_cast<SHORT>(24 + (iteration % 20))};
        if (!session.resize(size)) {
            reporter.check(false, "200 resize operations");
            return false;
        }
    }
    if (!session.resize(finalSize)) {
        reporter.check(false, "final resize operation");
        return false;
    }
    const auto reportStarted = Clock::now();
    const auto reportDeadline = reportStarted + 1s;
    bool observed = false;
    do {
        session.send("s");
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                reportDeadline - Clock::now());
        if (remaining <= 0ms) {
            break;
        }
        observed = session.waitForOutput(
            "SIZE 132 43",
            std::min(remaining, 50ms));
    } while (!observed && Clock::now() < reportDeadline);
    const auto reportMilliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            Clock::now() - reportStarted)
            .count();
    reporter.metric(
        "full.final_resize_report_ms",
        reportMilliseconds);
    if (!reporter.check(
            observed && reportMilliseconds <= 1000,
            "200 resizes and final dimensions")) {
        std::cout << "[DETAIL] " << session.diagnosticState()
                  << " output_hex=" << session.outputPreview()
                  << std::endl;
        return false;
    }
    session.send("q");
    const CloseResult result = session.finish(CloseMode::Natural);
    return verifyCloseResult(
        reporter,
        result,
        "full resize session",
        false,
        2000);
}

bool runFullSuite(
    Reporter& reporter,
    const ConPtyApi& api,
    const std::wstring& child)
{
    for (int iteration = 0; iteration < 5; ++iteration) {
        CloseResult result;
        if (!reporter.check(
                runCooperativeIteration(api, child, result),
                "warm-up cooperative close")) {
            return false;
        }
    }

    const DWORD baselineHandles = currentHandleCount();
    std::vector<DWORD> handleSamples;
    long long maximumCooperativeMilliseconds = 0;
    long long maximumForcedMilliseconds = 0;

    if (!run64MiBValidation(reporter, api, child)
        || !runFullResizeValidation(reporter, api, child)) {
        return false;
    }

    for (int iteration = 0; iteration < 100; ++iteration) {
        CloseResult result;
        if (!runCooperativeIteration(api, child, result)) {
            reporter.check(false, "100 cooperative close iterations");
            return false;
        }
        maximumCooperativeMilliseconds = std::max(
            maximumCooperativeMilliseconds,
            result.elapsedMilliseconds);
        handleSamples.push_back(currentHandleCount());
        if ((iteration + 1) % 10 == 0) {
            std::cout << "[PROGRESS] cooperative "
                      << (iteration + 1) << "/100" << std::endl;
        }
    }
    reporter.check(true, "100 cooperative close iterations");

    for (int iteration = 0; iteration < 100; ++iteration) {
        CloseResult result;
        if (!runForcedIteration(api, child, result)) {
            reporter.check(false, "100 forced close iterations");
            return false;
        }
        maximumForcedMilliseconds = std::max(
            maximumForcedMilliseconds,
            result.elapsedMilliseconds);
        handleSamples.push_back(currentHandleCount());
        if ((iteration + 1) % 10 == 0) {
            std::cout << "[PROGRESS] forced "
                      << (iteration + 1) << "/100" << std::endl;
        }
    }
    reporter.check(true, "100 forced close iterations");

    reporter.metric(
        "full.cooperative_close_max_ms",
        maximumCooperativeMilliseconds);
    reporter.metric(
        "full.forced_close_max_ms",
        maximumForcedMilliseconds);

    const DWORD finalHandles = waitForSettledHandleCount(
        baselineHandles + 4,
        5s);
    reporter.metric("full.handle_baseline", baselineHandles);
    reporter.metric("full.handle_final", finalHandles);
    reporter.metric(
        "full.handle_delta",
        static_cast<long long>(finalHandles)
            - static_cast<long long>(baselineHandles));

    std::size_t handleGrowthEvents = 0;
    std::size_t handleDeclineEvents = 0;
    for (std::size_t index = 1;
         index < handleSamples.size();
         ++index) {
        if (handleSamples[index] > handleSamples[index - 1]) {
            ++handleGrowthEvents;
        } else if (
            handleSamples[index] < handleSamples[index - 1]) {
            ++handleDeclineEvents;
        }
    }
    const bool sustainedMonotonicGrowth =
        handleGrowthEvents >= 2
        && handleDeclineEvents == 0;
    reporter.metric(
        "full.handle_growth_events",
        static_cast<long long>(handleGrowthEvents));
    reporter.metric(
        "full.handle_decline_events",
        static_cast<long long>(handleDeclineEvents));

    if (!reporter.check(
            gWorkerThreadCount.load() == 0,
            "full suite own worker count returns to zero")) {
        return false;
    }
    if (!reporter.check(
            finalHandles <= baselineHandles + 4,
            "full suite handle delta within four")) {
        return false;
    }
    if (!reporter.check(
            !sustainedMonotonicGrowth,
            "full suite handle samples have no monotonic growth")) {
        return false;
    }

    return reporter.failureCount() == 0;
}

std::wstring optionValue(
    int argumentCount,
    wchar_t** arguments,
    std::wstring_view option)
{
    for (int index = 1; index + 1 < argumentCount; ++index) {
        if (arguments[index] == option) {
            return arguments[index + 1];
        }
    }
    return {};
}

bool parseBrokerIterations(
    int argumentCount,
    wchar_t** arguments,
    int& iterations)
{
    constexpr int kDefaultIterations = 5;
    constexpr int kMaximumIterations = 100;
    iterations = kDefaultIterations;
    const std::wstring value = optionValue(
        argumentCount,
        arguments,
        L"--broker-iterations");
    if (value.empty()) {
        return true;
    }
    try {
        std::size_t parsed = 0;
        const int candidate = std::stoi(value, &parsed, 10);
        if (parsed != value.size()
            || candidate < 1
            || candidate > kMaximumIterations) {
            return false;
        }
        iterations = candidate;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::optional<HANDLE> inheritedHandleOption(
    int argumentCount,
    wchar_t** arguments,
    std::wstring_view option)
{
    const std::wstring value = optionValue(
        argumentCount,
        arguments,
        option);
    if (value.empty()) {
        return std::nullopt;
    }
    try {
        std::size_t parsed = 0;
        const unsigned long long numeric = std::stoull(value, &parsed, 10);
        if (parsed != value.size() || numeric == 0) {
            return std::nullopt;
        }
        return reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(numeric));
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

int runBrokerExecutable(
    int argumentCount,
    wchar_t** arguments)
{
    const auto commandHandle = inheritedHandleOption(
        argumentCount,
        arguments,
        L"--command-handle");
    const auto eventHandle = inheritedHandleOption(
        argumentCount,
        arguments,
        L"--event-handle");
    if (!commandHandle || !eventHandle) {
        std::cerr
            << "Usage: DirBridgeConPtyBrokerPrototype "
               "--command-handle <value> --event-handle <value>"
            << std::endl;
        return 64;
    }
    UniqueHandle command(*commandHandle);
    UniqueHandle event(*eventHandle);

    std::string error;
    std::vector<std::uint8_t> commandBytes;
    if (!readToEnd(command.get(), commandBytes, error)) {
        std::cerr << error << std::endl;
        return 65;
    }
    std::vector<BrokerMessage> messages;
    if (!decodeBrokerMessages(commandBytes, messages, error)
        || messages.size() != 1
        || messages[0].frame.type != BrokerFrameType::Start
        || messages[0].frame.generation == 0
        || messages[0].frame.sequence != 1) {
        const std::string detail = error.empty()
            ? "invalid broker start sequence"
            : error;
        const std::vector<std::uint8_t> payload(
            detail.begin(),
            detail.end());
        writeAll(
            event.get(),
            encodeBrokerMessage(
                BrokerFrame{BrokerFrameType::Error, 0, 1},
                payload),
            error);
        return 65;
    }

    const BrokerFrame start = messages[0].frame;
    const auto child = fromUtf8(messages[0].payload);
    if (!child) {
        error = "start payload was not valid UTF-8";
    } else {
        const std::filesystem::path childPath(*child);
        if (!childPath.is_absolute()
            || !std::filesystem::is_regular_file(childPath)) {
            error = "start payload did not name an absolute test child";
        }
    }

    ConPtyApi api;
    if (error.empty() && !api.load()) {
        error = api.error;
    }
    if (!error.empty()) {
        const std::vector<std::uint8_t> payload(
            error.begin(),
            error.end());
        std::string writeError;
        writeAll(
            event.get(),
            encodeBrokerMessage(
                BrokerFrame{
                    BrokerFrameType::Error,
                    start.generation,
                    1},
                payload),
            writeError);
        return 2;
    }

    if (!writeAll(
            event.get(),
            encodeBrokerMessage(
                BrokerFrame{
                    BrokerFrameType::Ready,
                    start.generation,
                    1},
                {}),
            error)) {
        std::cerr << error << std::endl;
        return 66;
    }

    Reporter reporter;
    if (!runBrokerWorkerSuite(reporter, api, *child)
        || reporter.failureCount() != 0) {
        const std::string detail = "broker ConPTY lifecycle failed";
        const std::vector<std::uint8_t> payload(
            detail.begin(),
            detail.end());
        std::string writeError;
        writeAll(
            event.get(),
            encodeBrokerMessage(
                BrokerFrame{
                    BrokerFrameType::Error,
                    start.generation,
                    2},
                payload),
            writeError);
        return 1;
    }

    if (!writeAll(
            event.get(),
            encodeBrokerMessage(
                BrokerFrame{
                    BrokerFrameType::Stopped,
                    start.generation,
                    2},
                {}),
            error)) {
        std::cerr << error << std::endl;
        return 66;
    }
    return 0;
}

int run(int argumentCount, wchar_t** arguments)
{
    const std::wstring child =
        optionValue(argumentCount, arguments, L"--child");
    const std::wstring suite =
        optionValue(argumentCount, arguments, L"--suite");
    const std::wstring broker =
        optionValue(argumentCount, arguments, L"--broker");
    int brokerIterations = 5;
    if (child.empty()
        || (suite != L"smoke"
            && suite != L"full"
            && suite != L"handle-diagnostic"
            && suite != L"handle-diagnostic-close-first"
            && suite != L"broker-worker"
            && suite != L"broker-diagnostic"
            && suite != L"broker-ipc-diagnostic")) {
        std::cerr
            << "Usage: DirBridgeConPtyChecks "
               "--child <absolute-path> "
               "--suite <smoke|full|handle-diagnostic|"
               "handle-diagnostic-close-first|broker-worker|"
               "broker-diagnostic|broker-ipc-diagnostic> "
               "[--broker <absolute-path>] "
               "[--broker-iterations <1-100>]"
            << std::endl;
        return 64;
    }

    const std::filesystem::path childPath(child);
    if (!childPath.is_absolute()
        || !std::filesystem::is_regular_file(childPath)) {
        std::cerr << "--child must name an existing absolute file"
                  << std::endl;
        return 64;
    }
    if (suite == L"broker-ipc-diagnostic") {
        const std::filesystem::path brokerPath(broker);
        if (broker.empty()
            || !brokerPath.is_absolute()
            || !std::filesystem::is_regular_file(brokerPath)) {
            std::cerr
                << "--broker must name an existing absolute file "
                   "for broker-ipc-diagnostic"
                << std::endl;
            return 64;
        }
        if (!parseBrokerIterations(
                argumentCount,
                arguments,
                brokerIterations)) {
            std::cerr
                << "--broker-iterations must be an integer from 1 to 100"
                << std::endl;
            return 64;
        }
    }

    ConPtyApi api;
    const bool parentNeedsConPty =
        suite != L"broker-diagnostic"
        && suite != L"broker-ipc-diagnostic";
    if (parentNeedsConPty && !api.load()) {
        std::cerr << api.error << std::endl;
        return 2;
    }

    Reporter reporter;
    bool passed = false;
    const char* suiteName = nullptr;
    if (suite == L"smoke") {
        suiteName = "smoke";
        passed = runSmokeSuite(reporter, api, child);
    } else if (suite == L"full") {
        suiteName = "full";
        passed = runFullSuite(reporter, api, child);
    } else if (suite == L"handle-diagnostic") {
        suiteName = "handle-diagnostic";
        passed = runHandleDiagnosticSuite(
            reporter,
            api,
            child,
            false);
    } else if (suite == L"handle-diagnostic-close-first") {
        suiteName = "handle-diagnostic-close-first";
        passed = runHandleDiagnosticSuite(
            reporter,
            api,
            child,
            true);
    } else if (suite == L"broker-worker") {
        suiteName = "broker-worker";
        passed = runBrokerWorkerSuite(reporter, api, child);
    } else if (suite == L"broker-diagnostic") {
        suiteName = "broker-diagnostic";
        passed = runBrokerDiagnosticSuite(reporter, child);
    } else {
        suiteName = "broker-ipc-diagnostic";
        passed = runBrokerIpcDiagnosticSuite(
            reporter,
            broker,
            child,
            brokerIterations);
    }
    std::cout << "[SUMMARY] suite=" << suiteName
              << " failures=" << reporter.failureCount()
              << std::endl;
    return passed && reporter.failureCount() == 0 ? 0 : 1;
}

} // namespace

int main()
{
    int argumentCount = 0;
    wchar_t** arguments =
        CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) {
        std::cerr << "CommandLineToArgvW failed" << std::endl;
        return 64;
    }
#ifdef DIRBRIDGE_CONPTY_BROKER_EXECUTABLE
    const int result = runBrokerExecutable(
        argumentCount,
        arguments);
#else
    const int result = run(argumentCount, arguments);
#endif
    LocalFree(arguments);
    return result;
}
