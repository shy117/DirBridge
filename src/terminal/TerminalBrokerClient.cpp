#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "terminal/TerminalBrokerClient.h"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

#ifndef PROC_THREAD_ATTRIBUTE_HANDLE_LIST
#define PROC_THREAD_ATTRIBUTE_HANDLE_LIST 0x00020002
#endif

namespace dirbridge::terminal::broker {
namespace {

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

class AttributeList
{
public:
    bool initialize()
    {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        storage_.resize(size);
        value_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        return InitializeProcThreadAttributeList(value_, 1, 0, &size) != FALSE;
    }

    ~AttributeList()
    {
        if (value_ != nullptr)
        {
            DeleteProcThreadAttributeList(value_);
        }
    }

    LPPROC_THREAD_ATTRIBUTE_LIST get() const noexcept { return value_; }

private:
    std::vector<std::uint8_t> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST value_ = nullptr;
};

std::string windowsError(const char *operation)
{
    std::ostringstream stream;
    stream << operation << " failed with Win32 error " << GetLastError();
    return stream.str();
}

std::wstring quote(const std::wstring &value)
{
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : value)
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

std::uint32_t read32(const std::uint8_t *bytes)
{
    return static_cast<std::uint32_t>(bytes[0])
        | (static_cast<std::uint32_t>(bytes[1]) << 8U)
        | (static_cast<std::uint32_t>(bytes[2]) << 16U)
        | (static_cast<std::uint32_t>(bytes[3]) << 24U);
}

void wipe(std::vector<std::uint8_t> &bytes) noexcept
{
    if (!bytes.empty())
    {
        SecureZeroMemory(bytes.data(), bytes.size());
        bytes.clear();
    }
}

} // namespace

struct TerminalBrokerClient::Impl
{
    Handle commandWrite;
    Handle eventRead;
    Handle process;
    Handle job;
    std::mutex commandMutex;
    std::uint32_t generation = 1;
    std::uint32_t nextCommandSequence = 1;
    std::uint32_t nextEventSequence = 1;
    std::string error;
    bool hasStarted = false;
    bool closeSent = false;

    bool send(FrameType type, std::vector<std::uint8_t> payload)
    {
        std::lock_guard<std::mutex> lock(commandMutex);
        if (!commandWrite || closeSent)
        {
            error = "broker command channel is closed";
            return false;
        }
        const std::vector<std::uint8_t> encoded = encodeFrame(
            {type, generation, nextCommandSequence, std::move(payload)});
        if (encoded.empty())
        {
            error = "broker command frame is invalid";
            return false;
        }
        if (!writeAll(commandWrite.get(), encoded))
        {
            const DWORD code = GetLastError();
            if (type == FrameType::Close
                && (code == ERROR_BROKEN_PIPE || code == ERROR_NO_DATA))
            {
                closeSent = true;
                commandWrite.reset();
                return true;
            }
            error = windowsError("WriteFile(broker command)");
            return false;
        }
        ++nextCommandSequence;
        if (type == FrameType::Close)
        {
            closeSent = true;
            commandWrite.reset();
        }
        return true;
    }
};

TerminalBrokerClient::TerminalBrokerClient()
    : impl_(std::make_unique<Impl>())
{
}

TerminalBrokerClient::~TerminalBrokerClient()
{
    terminate();
}

bool TerminalBrokerClient::start(
    const std::filesystem::path &brokerExecutable,
    const StartRequest &request)
{
    return start(
        brokerExecutable,
        request,
        StoredPasswordLease(std::string_view{}));
}

bool TerminalBrokerClient::start(
    const std::filesystem::path &brokerExecutable,
    const StartRequest &request,
    StoredPasswordLease password)
{
    if (impl_->hasStarted || brokerExecutable.empty())
    {
        impl_->error = "broker client start arguments are invalid";
        return false;
    }
    const bool requiresPassword = request.ssh.authentication
        == SshAuthenticationMode::StoredPassword;
    if (requiresPassword == password.empty())
    {
        impl_->error = "broker password mode and password lease do not match";
        return false;
    }

    std::vector<std::uint8_t> commands;
    const auto appendFrame = [&commands](const Frame &frame) {
        const std::vector<std::uint8_t> encoded = encodeFrame(frame);
        if (encoded.empty())
        {
            return false;
        }
        commands.insert(commands.end(), encoded.begin(), encoded.end());
        return true;
    };
    const std::vector<std::uint8_t> startPayload = encodeStartRequest(request);
    if (startPayload.empty() || !appendFrame({
            FrameType::Start,
            impl_->generation,
            impl_->nextCommandSequence++,
            startPayload}))
    {
        impl_->error = "failed to encode broker Start frame";
        return false;
    }
    if (requiresPassword && !password.consume(
            [&](const char *bytes, std::size_t size) {
                return appendFrame({
                    FrameType::AuthSecret,
                    impl_->generation,
                    impl_->nextCommandSequence++,
                    std::vector<std::uint8_t>(bytes, bytes + size)});
            }))
    {
        wipe(commands);
        impl_->error = "failed to encode broker AuthSecret frame";
        return false;
    }

    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE commandReadRaw = nullptr;
    HANDLE commandWriteRaw = nullptr;
    HANDLE eventReadRaw = nullptr;
    HANDLE eventWriteRaw = nullptr;
    if (!CreatePipe(&commandReadRaw, &commandWriteRaw, &inheritable, 0))
    {
        wipe(commands);
        impl_->error = windowsError("CreatePipe(broker command)");
        return false;
    }
    Handle commandRead(commandReadRaw);
    impl_->commandWrite.reset(commandWriteRaw);
    if (!CreatePipe(&eventReadRaw, &eventWriteRaw, &inheritable, 0))
    {
        wipe(commands);
        impl_->error = windowsError("CreatePipe(broker event)");
        return false;
    }
    impl_->eventRead.reset(eventReadRaw);
    Handle eventWrite(eventWriteRaw);
    if (!SetHandleInformation(
            impl_->commandWrite.get(), HANDLE_FLAG_INHERIT, 0)
        || !SetHandleInformation(
            impl_->eventRead.get(), HANDLE_FLAG_INHERIT, 0))
    {
        wipe(commands);
        impl_->error = windowsError("SetHandleInformation(broker)");
        return false;
    }

    impl_->job.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!impl_->job || !SetInformationJobObject(
            impl_->job.get(),
            JobObjectExtendedLimitInformation,
            &limit,
            sizeof(limit)))
    {
        wipe(commands);
        impl_->error = windowsError("SetInformationJobObject(broker)");
        return false;
    }

    AttributeList attributes;
    if (!attributes.initialize())
    {
        wipe(commands);
        impl_->error = windowsError("InitializeProcThreadAttributeList(broker)");
        return false;
    }
    HANDLE inherited[] = {commandRead.get(), eventWrite.get()};
    if (!UpdateProcThreadAttribute(
            attributes.get(),
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited,
            sizeof(inherited),
            nullptr,
            nullptr))
    {
        wipe(commands);
        impl_->error = windowsError("UpdateProcThreadAttribute(broker)");
        return false;
    }

    std::wstring commandLine = quote(brokerExecutable.wstring())
        + L" --command-handle "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(commandRead.get()))
        + L" --event-handle "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(eventWrite.get()));
    std::vector<wchar_t> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back(L'\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes.get();
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            brokerExecutable.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT
                | EXTENDED_STARTUPINFO_PRESENT,
            nullptr,
            nullptr,
            &startup.StartupInfo,
            &process))
    {
        wipe(commands);
        impl_->error = windowsError("CreateProcessW(broker)");
        return false;
    }
    impl_->process.reset(process.hProcess);
    Handle primaryThread(process.hThread);
    if (!AssignProcessToJobObject(impl_->job.get(), impl_->process.get())
        || ResumeThread(primaryThread.get()) == static_cast<DWORD>(-1))
    {
        wipe(commands);
        impl_->error = windowsError("start broker in Job");
        TerminateJobObject(impl_->job.get(), 0xE201);
        return false;
    }
    commandRead.reset();
    eventWrite.reset();
    if (!writeAll(impl_->commandWrite.get(), commands))
    {
        wipe(commands);
        impl_->error = windowsError("WriteFile(initial broker commands)");
        TerminateJobObject(impl_->job.get(), 0xE202);
        return false;
    }
    wipe(commands);
    impl_->hasStarted = true;
    return true;
}

bool TerminalBrokerClient::sendInput(const std::vector<std::uint8_t> &bytes)
{
    if (bytes.empty())
    {
        return true;
    }
    return impl_->send(FrameType::Input, bytes);
}

bool TerminalBrokerClient::resize(std::uint16_t columns, std::uint16_t rows)
{
    if (columns == 0 || rows == 0)
    {
        impl_->error = "broker resize dimensions are invalid";
        return false;
    }
    return impl_->send(FrameType::Resize, {
        static_cast<std::uint8_t>(columns),
        static_cast<std::uint8_t>(columns >> 8U),
        static_cast<std::uint8_t>(rows),
        static_cast<std::uint8_t>(rows >> 8U)});
}

bool TerminalBrokerClient::close()
{
    if (impl_->closeSent)
    {
        return true;
    }
    return impl_->send(FrameType::Close, {});
}

EventReadResult TerminalBrokerClient::readEvent(Frame &frame)
{
    if (!impl_->eventRead)
    {
        impl_->error = "broker event channel is closed";
        return EventReadResult::Error;
    }
    std::array<std::uint8_t, FrameHeaderSize> header{};
    if (!readExact(impl_->eventRead.get(), header.data(), header.size()))
    {
        const DWORD code = GetLastError();
        if (code == ERROR_BROKEN_PIPE || code == ERROR_OPERATION_ABORTED)
        {
            return EventReadResult::Eof;
        }
        impl_->error = windowsError("ReadFile(broker event header)");
        return EventReadResult::Error;
    }
    const std::uint32_t payloadSize = read32(header.data() + 20);
    if (payloadSize > MaximumPayloadSize)
    {
        impl_->error = "broker event payload exceeded limit";
        return EventReadResult::Error;
    }
    std::vector<std::uint8_t> encoded(header.begin(), header.end());
    encoded.resize(FrameHeaderSize + payloadSize);
    if (payloadSize > 0 && !readExact(
            impl_->eventRead.get(),
            encoded.data() + FrameHeaderSize,
            payloadSize))
    {
        impl_->error = "broker event payload was truncated";
        return EventReadResult::Error;
    }
    std::vector<Frame> decoded;
    if (!decodeFrames(encoded, decoded, impl_->error) || decoded.size() != 1)
    {
        return EventReadResult::Error;
    }
    frame = std::move(decoded.front());
    if (!isEventFrame(frame.type)
        || frame.generation != impl_->generation
        || frame.sequence != impl_->nextEventSequence++)
    {
        impl_->error = "broker event generation or sequence is invalid";
        return EventReadResult::Error;
    }
    return EventReadResult::Event;
}

bool TerminalBrokerClient::waitForBroker(
    std::chrono::milliseconds timeout,
    std::uint32_t &exitCode)
{
    if (!impl_->process)
    {
        impl_->error = "broker process is not running";
        return false;
    }
    const DWORD milliseconds = timeout.count() > MAXDWORD
        ? MAXDWORD
        : static_cast<DWORD>(timeout.count());
    const DWORD result = WaitForSingleObject(impl_->process.get(), milliseconds);
    if (result != WAIT_OBJECT_0)
    {
        impl_->error = result == WAIT_TIMEOUT
            ? "broker did not exit before timeout"
            : windowsError("WaitForSingleObject(broker)");
        return false;
    }
    DWORD nativeExitCode = 0;
    if (!GetExitCodeProcess(impl_->process.get(), &nativeExitCode))
    {
        impl_->error = windowsError("GetExitCodeProcess(broker)");
        return false;
    }
    exitCode = nativeExitCode;
    impl_->process.reset();
    impl_->commandWrite.reset();
    impl_->eventRead.reset();
    return true;
}

void TerminalBrokerClient::terminate(std::uint32_t exitCode) noexcept
{
    std::lock_guard<std::mutex> lock(impl_->commandMutex);
    impl_->commandWrite.reset();
    impl_->eventRead.reset();
    if (impl_->job)
    {
        TerminateJobObject(impl_->job.get(), exitCode);
    }
    if (impl_->process)
    {
        WaitForSingleObject(impl_->process.get(), 2000);
        impl_->process.reset();
    }
    impl_->job.reset();
}

bool TerminalBrokerClient::started() const noexcept
{
    return impl_->hasStarted;
}

const std::string &TerminalBrokerClient::error() const noexcept
{
    return impl_->error;
}

} // namespace dirbridge::terminal::broker
