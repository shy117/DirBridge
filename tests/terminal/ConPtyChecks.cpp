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
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "ConPtyTestProtocol.h"

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
using dirbridge::terminal::test::kOutputBlockCount;
using dirbridge::terminal::test::kOutputBlockSize;
using dirbridge::terminal::test::kOutputByteCount;
using dirbridge::terminal::test::kOutputTailMarker;
using dirbridge::terminal::test::makeOutputBlock;

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

    CloseResult finish(CloseMode mode)
    {
        CloseResult result;
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
        requestPseudoConsoleClose();

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
    CloseResult& result)
{
    ConPtySession session(api);
    if (!session.start(child, L"cooperative-close")
        || !session.waitForOutput("READY COOPERATIVE", 2s)) {
        return false;
    }
    result = session.finish(CloseMode::Cooperative);
    return result.completed
        && !result.escalated
        && result.elapsedMilliseconds <= 2000
        && result.activeProcesses == 0;
}

bool runForcedIteration(
    const ConPtyApi& api,
    const std::wstring& child,
    CloseResult& result)
{
    ConPtySession session(api);
    if (!session.start(child, L"stubborn-close")
        || !session.waitForOutput("READY STUBBORN", 2s)) {
        return false;
    }
    result = session.finish(CloseMode::Forced);
    return result.completed
        && result.escalated
        && result.elapsedMilliseconds <= 5000
        && result.activeProcesses == 0;
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

int run(int argumentCount, wchar_t** arguments)
{
    const std::wstring child =
        optionValue(argumentCount, arguments, L"--child");
    const std::wstring suite =
        optionValue(argumentCount, arguments, L"--suite");
    if (child.empty()
        || (suite != L"smoke" && suite != L"full")) {
        std::cerr
            << "Usage: DirBridgeConPtyChecks "
               "--child <absolute-path> --suite <smoke|full>"
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

    ConPtyApi api;
    if (!api.load()) {
        std::cerr << api.error << std::endl;
        return 2;
    }

    Reporter reporter;
    const bool passed = suite == L"smoke"
        ? runSmokeSuite(reporter, api, child)
        : runFullSuite(reporter, api, child);
    std::cout << "[SUMMARY] suite="
              << (suite == L"smoke" ? "smoke" : "full")
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
    const int result = run(argumentCount, arguments);
    LocalFree(arguments);
    return result;
}
