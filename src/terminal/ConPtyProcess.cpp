#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "terminal/ConPtyProcess.h"
#include "terminal/OpenSshLauncher.h"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <sstream>
#include <thread>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace dirbridge::terminal {
namespace {

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
    explicit operator bool() const noexcept
    {
        return value_ != nullptr && value_ != INVALID_HANDLE_VALUE;
    }
    HANDLE release() noexcept
    {
        const HANDLE value = value_;
        value_ = nullptr;
        return value;
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

struct ConPtyApi
{
    using CreateFunction = HRESULT(WINAPI*)(
        COORD, HANDLE, HANDLE, DWORD, HANDLE*);
    using ResizeFunction = HRESULT(WINAPI*)(HANDLE, COORD);
    using CloseFunction = void(WINAPI*)(HANDLE);

    CreateFunction create = nullptr;
    ResizeFunction resize = nullptr;
    CloseFunction close = nullptr;

    bool load()
    {
        HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
        create = reinterpret_cast<CreateFunction>(
            GetProcAddress(kernel, "CreatePseudoConsole"));
        resize = reinterpret_cast<ResizeFunction>(
            GetProcAddress(kernel, "ResizePseudoConsole"));
        close = reinterpret_cast<CloseFunction>(
            GetProcAddress(kernel, "ClosePseudoConsole"));
        return create && resize && close;
    }
};

std::string windowsError(const char *operation)
{
    std::ostringstream stream;
    stream << operation << " failed with Win32 error " << GetLastError();
    return stream.str();
}

bool environmentNameEquals(
    const std::wstring &entry,
    const std::wstring &name)
{
    const std::size_t separator = entry.find(L'=');
    if (separator == std::wstring::npos || separator != name.size())
    {
        return false;
    }
    return CompareStringOrdinal(
               entry.data(),
               static_cast<int>(separator),
               name.data(),
               static_cast<int>(name.size()),
               TRUE)
        == CSTR_EQUAL;
}

std::vector<wchar_t> makeEnvironment(
    const std::vector<std::pair<std::wstring, std::wstring>> &overrides)
{
    std::vector<std::wstring> entries;
    LPWCH raw = GetEnvironmentStringsW();
    if (raw != nullptr)
    {
        for (const wchar_t *cursor = raw; *cursor != L'\0';)
        {
            std::wstring entry(cursor);
            entries.push_back(entry);
            cursor += entry.size() + 1;
        }
        FreeEnvironmentStringsW(raw);
    }
    for (const auto &[name, value] : overrides)
    {
        entries.erase(
            std::remove_if(
                entries.begin(),
                entries.end(),
                [&name](const std::wstring &entry) {
                    return environmentNameEquals(entry, name);
                }),
            entries.end());
        entries.push_back(name + L"=" + value);
    }
    std::sort(entries.begin(), entries.end(), [](const auto &left, const auto &right) {
        return _wcsicmp(left.c_str(), right.c_str()) < 0;
    });

    std::vector<wchar_t> block;
    for (const std::wstring &entry : entries)
    {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

} // namespace

struct ConPtyProcess::Impl
{
    ConPtyApi api;
    Handle input;
    Handle output;
    Handle process;
    Handle job;
    HANDLE pseudoConsole = nullptr;
    std::thread outputThread;
    std::mutex inputMutex;
    std::mutex outputMutex;
    std::mutex stateMutex;
    std::vector<std::uint8_t> outputBytes;
    std::string error;
    bool started = false;
    bool finished = false;

    void closePseudoConsole() noexcept
    {
        if (pseudoConsole != nullptr)
        {
            api.close(pseudoConsole);
            pseudoConsole = nullptr;
        }
    }

    void joinOutput() noexcept
    {
        if (outputThread.joinable())
        {
            outputThread.join();
        }
    }

    void cleanup() noexcept
    {
        input.reset();
        if (process && WaitForSingleObject(process.get(), 0) == WAIT_TIMEOUT)
        {
            if (job)
            {
                TerminateJobObject(job.get(), 0xE100);
            }
            else
            {
                TerminateProcess(process.get(), 0xE100);
            }
            WaitForSingleObject(process.get(), 2000);
        }
        closePseudoConsole();
        joinOutput();
        output.reset();
        process.reset();
        job.reset();
        finished = true;
    }
};

ConPtyProcess::ConPtyProcess() : impl_(std::make_unique<Impl>()) {}

ConPtyProcess::~ConPtyProcess()
{
    impl_->cleanup();
}

bool ConPtyProcess::start(
    const OpenSshLaunchSpec &spec,
    const std::vector<std::pair<std::wstring, std::wstring>>
        &environmentOverrides,
    std::uint16_t columns,
    std::uint16_t rows)
{
    if (impl_->started || columns == 0 || rows == 0)
    {
        impl_->error = "ConPTY process start arguments are invalid";
        return false;
    }
    impl_->started = true;
    if (!impl_->api.load())
    {
        impl_->error = "ConPTY API is unavailable";
        return false;
    }

    HANDLE pseudoInput = nullptr;
    HANDLE inputWrite = nullptr;
    if (!CreatePipe(&pseudoInput, &inputWrite, nullptr, 0))
    {
        impl_->error = windowsError("CreatePipe(input)");
        return false;
    }
    Handle pseudoInputHandle(pseudoInput);
    impl_->input.reset(inputWrite);

    HANDLE outputRead = nullptr;
    HANDLE pseudoOutput = nullptr;
    if (!CreatePipe(&outputRead, &pseudoOutput, nullptr, 0))
    {
        impl_->error = windowsError("CreatePipe(output)");
        impl_->cleanup();
        return false;
    }
    impl_->output.reset(outputRead);
    Handle pseudoOutputHandle(pseudoOutput);

    const COORD size{
        static_cast<SHORT>(columns),
        static_cast<SHORT>(rows)};
    const HRESULT created = impl_->api.create(
        size,
        pseudoInputHandle.get(),
        pseudoOutputHandle.get(),
        0,
        &impl_->pseudoConsole);
    if (FAILED(created))
    {
        impl_->error = "CreatePseudoConsole failed";
        impl_->cleanup();
        return false;
    }

    impl_->job.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!impl_->job
        || !SetInformationJobObject(
            impl_->job.get(),
            JobObjectExtendedLimitInformation,
            &limit,
            sizeof(limit)))
    {
        impl_->error = windowsError("configure SSH job");
        impl_->cleanup();
        return false;
    }

    SIZE_T attributeSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
    std::vector<std::uint8_t> attributeStorage(attributeSize);
    auto *attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        attributeStorage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &attributeSize))
    {
        impl_->error = windowsError("InitializeProcThreadAttributeList");
        impl_->cleanup();
        return false;
    }
    struct AttributeGuard
    {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeGuard() { DeleteProcThreadAttributeList(value); }
    } attributeGuard{attributes};
    if (!UpdateProcThreadAttribute(
            attributes,
            0,
            PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
            impl_->pseudoConsole,
            sizeof(impl_->pseudoConsole),
            nullptr,
            nullptr))
    {
        impl_->error = windowsError("UpdateProcThreadAttribute(ConPTY)");
        impl_->cleanup();
        return false;
    }

    std::wstring commandLine = OpenSshLauncher::serializeCommandLine(spec);
    std::vector<wchar_t> mutableCommandLine(
        commandLine.begin(),
        commandLine.end());
    mutableCommandLine.push_back(L'\0');
    std::vector<wchar_t> environment = makeEnvironment(environmentOverrides);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            spec.executablePath.c_str(),
            mutableCommandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            EXTENDED_STARTUPINFO_PRESENT
                | CREATE_SUSPENDED
                | CREATE_UNICODE_ENVIRONMENT,
            environment.data(),
            spec.workingDirectory.c_str(),
            &startup.StartupInfo,
            &process))
    {
        impl_->error = windowsError("CreateProcessW(ssh.exe)");
        impl_->cleanup();
        return false;
    }
    impl_->process.reset(process.hProcess);
    Handle primaryThread(process.hThread);
    if (!AssignProcessToJobObject(impl_->job.get(), impl_->process.get()))
    {
        impl_->error = windowsError("AssignProcessToJobObject(ssh.exe)");
        impl_->cleanup();
        return false;
    }

    pseudoInputHandle.reset();
    pseudoOutputHandle.reset();
    impl_->outputThread = std::thread([this] {
        std::array<std::uint8_t, 4096> buffer{};
        for (;;)
        {
            DWORD read = 0;
            if (!ReadFile(
                    impl_->output.get(),
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &read,
                    nullptr)
                || read == 0)
            {
                break;
            }
            std::lock_guard<std::mutex> lock(impl_->outputMutex);
            impl_->outputBytes.insert(
                impl_->outputBytes.end(),
                buffer.begin(),
                buffer.begin() + read);
        }
    });
    if (ResumeThread(primaryThread.get()) == static_cast<DWORD>(-1))
    {
        impl_->error = windowsError("ResumeThread(ssh.exe)");
        impl_->cleanup();
        return false;
    }
    return true;
}

bool ConPtyProcess::send(const std::vector<std::uint8_t> &bytes)
{
    std::lock_guard<std::mutex> lock(impl_->inputMutex);
    std::size_t offset = 0;
    while (offset < bytes.size())
    {
        DWORD written = 0;
        const DWORD size = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size() - offset,
            std::numeric_limits<DWORD>::max()));
        if (!WriteFile(
                impl_->input.get(),
                bytes.data() + offset,
                size,
                &written,
                nullptr)
            || written == 0)
        {
            impl_->error = windowsError("WriteFile(ConPTY input)");
            return false;
        }
        offset += written;
    }
    return true;
}

bool ConPtyProcess::resize(std::uint16_t columns, std::uint16_t rows)
{
    std::lock_guard<std::mutex> lock(impl_->stateMutex);
    if (!impl_->pseudoConsole || columns == 0 || rows == 0)
    {
        impl_->error = "ConPTY resize arguments are invalid";
        return false;
    }
    const COORD size{
        static_cast<SHORT>(columns),
        static_cast<SHORT>(rows)};
    if (FAILED(impl_->api.resize(impl_->pseudoConsole, size)))
    {
        impl_->error = "ResizePseudoConsole failed";
        return false;
    }
    return true;
}

void ConPtyProcess::closeInput() noexcept
{
    std::lock_guard<std::mutex> lock(impl_->inputMutex);
    impl_->input.reset();
}

bool ConPtyProcess::hasExited() const noexcept
{
    return impl_->process
        && WaitForSingleObject(impl_->process.get(), 0) == WAIT_OBJECT_0;
}

bool ConPtyProcess::wait(
    std::chrono::milliseconds timeout,
    std::uint32_t &exitCode)
{
    const DWORD milliseconds = timeout.count() > MAXDWORD
        ? MAXDWORD
        : static_cast<DWORD>(timeout.count());
    const DWORD result = WaitForSingleObject(impl_->process.get(), milliseconds);
    if (result != WAIT_OBJECT_0)
    {
        impl_->error = result == WAIT_TIMEOUT
            ? "ssh.exe did not exit before timeout"
            : windowsError("WaitForSingleObject(ssh.exe)");
        return false;
    }
    DWORD nativeExitCode = 0;
    if (!GetExitCodeProcess(impl_->process.get(), &nativeExitCode))
    {
        impl_->error = windowsError("GetExitCodeProcess(ssh.exe)");
        return false;
    }
    exitCode = nativeExitCode;
    {
        std::scoped_lock lock(impl_->inputMutex, impl_->stateMutex);
        impl_->input.reset();
        impl_->closePseudoConsole();
        impl_->joinOutput();
        impl_->output.reset();
        impl_->process.reset();
        impl_->job.reset();
        impl_->finished = true;
    }
    return true;
}

void ConPtyProcess::terminate(std::uint32_t exitCode) noexcept
{
    std::scoped_lock lock(impl_->inputMutex, impl_->stateMutex);
    if (impl_->job)
    {
        TerminateJobObject(impl_->job.get(), exitCode);
    }
    impl_->cleanup();
}

std::vector<std::uint8_t> ConPtyProcess::takeOutput()
{
    std::lock_guard<std::mutex> lock(impl_->outputMutex);
    return std::move(impl_->outputBytes);
}

const std::string &ConPtyProcess::error() const noexcept
{
    return impl_->error;
}

} // namespace dirbridge::terminal
