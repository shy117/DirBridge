#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "terminal/StoredPasswordChannel.h"
#include "terminal/StoredPasswordLease.h"

#include <array>
#include <iostream>
#include <string>

namespace {

using dirbridge::terminal::StoredPasswordLease;
using dirbridge::terminal::StoredPasswordChannel;

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

    HANDLE get() const { return value_; }
    HANDLE release()
    {
        const HANDLE value = value_;
        value_ = INVALID_HANDLE_VALUE;
        return value;
    }
    void reset(HANDLE value = INVALID_HANDLE_VALUE)
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

class Reporter
{
public:
    void check(bool condition, const char *label)
    {
        std::cout << (condition ? "[PASS] " : "[FAIL] ")
                  << label << std::endl;
        if (!condition)
        {
            ++failures_;
        }
    }

    int failures() const { return failures_; }

private:
    int failures_ = 0;
};

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
    value.resize(written);
    return value;
}

class EnvironmentGuard
{
public:
    EnvironmentGuard(const wchar_t *name, const std::wstring &value)
        : name_(name), previous_(readEnvironment(name)), hadValue_(!previous_.empty())
    {
        SetEnvironmentVariableW(name_.c_str(), value.c_str());
    }

    ~EnvironmentGuard()
    {
        SetEnvironmentVariableW(
            name_.c_str(),
            hadValue_ ? previous_.c_str() : nullptr);
    }

private:
    std::wstring name_;
    std::wstring previous_;
    bool hadValue_ = false;
};

struct Child
{
    Handle process;
    Handle output;
};

Child startHelper(
    const std::wstring &helper,
    const std::wstring &pipeName,
    const std::wstring &token,
    const std::wstring &prompt)
{
    SECURITY_ATTRIBUTES inheritable{};
    inheritable.nLength = sizeof(inheritable);
    inheritable.bInheritHandle = TRUE;
    HANDLE outputReadRaw = INVALID_HANDLE_VALUE;
    HANDLE outputWriteRaw = INVALID_HANDLE_VALUE;
    if (!CreatePipe(
            &outputReadRaw,
            &outputWriteRaw,
            &inheritable,
            0))
    {
        return {};
    }
    Handle outputRead(outputReadRaw);
    Handle outputWrite(outputWriteRaw);
    SetHandleInformation(outputRead.get(), HANDLE_FLAG_INHERIT, 0);

    EnvironmentGuard pipeGuard(L"DIRBRIDGE_SSH_ASKPASS_PIPE", pipeName);
    EnvironmentGuard tokenGuard(L"DIRBRIDGE_SSH_ASKPASS_TOKEN", token);
    EnvironmentGuard promptGuard(L"SSH_ASKPASS_PROMPT", prompt);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = outputWrite.get();
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    std::wstring commandLine = L"\"" + helper + L"\"";
    if (!CreateProcessW(
            helper.c_str(),
            commandLine.data(),
            nullptr,
            nullptr,
            TRUE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &startup,
            &process))
    {
        return {};
    }
    CloseHandle(process.hThread);
    outputWrite.reset();
    return {Handle(process.hProcess), std::move(outputRead)};
}

std::string collectOutput(HANDLE output)
{
    std::string result;
    std::array<char, 256> buffer{};
    for (;;)
    {
        DWORD received = 0;
        if (!ReadFile(
                output,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &received,
                nullptr)
            || received == 0)
        {
            break;
        }
        result.append(buffer.data(), received);
    }
    return result;
}

DWORD waitForExit(HANDLE process)
{
    if (WaitForSingleObject(process, 10000) != WAIT_OBJECT_0)
    {
        TerminateProcess(process, 99);
        return 99;
    }
    DWORD code = 99;
    GetExitCodeProcess(process, &code);
    return code;
}

} // namespace

int main()
{
    Reporter reporter;
    int argc = 0;
    wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc != 2)
    {
        std::cerr << "usage: AskPassChecks <helper>" << std::endl;
        if (argv != nullptr)
        {
            LocalFree(argv);
        }
        return 2;
    }

    const std::wstring helper(argv[1]);
    LocalFree(argv);
    const DWORD helperAttributes = GetFileAttributesW(helper.c_str());
    reporter.check(
        helperAttributes != INVALID_FILE_ATTRIBUTES
            && (helperAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0,
        "AskPass helper exists");

    std::string channelError;
    auto channel = StoredPasswordChannel::create(
        StoredPasswordLease("ubuntu-secret"),
        std::filesystem::path(helper),
        channelError);
    reporter.check(
        channel != nullptr,
        "create private one-shot AskPass channel");
    if (!channel)
    {
        std::cout << "[DETAIL] " << channelError << std::endl;
        return 1;
    }

    Child child = startHelper(
        helper,
        channel->endpoint().pipeName,
        channel->endpoint().token,
        L"");
    reporter.check(
        child.process.get() != INVALID_HANDLE_VALUE,
        "start AskPass helper without confirmation hint");

    const bool sent = channel->serveOnce(channelError);
    reporter.check(sent, "broker authenticates helper and sends password once");
    std::string repeatedError;
    reporter.check(
        !channel->serveOnce(repeatedError),
        "broker rejects a second password request");

    const DWORD exitCode = waitForExit(child.process.get());
    const std::string output = collectOutput(child.output.get());
    reporter.check(exitCode == 0, "AskPass helper exits successfully");
    reporter.check(
        output == "ubuntu-secret\n",
        "AskPass helper returns only the stored password");

    Child confirmChild = startHelper(
        helper,
        L"\\\\.\\pipe\\DirBridge.SshAskPass.missing",
        channel->endpoint().token,
        L"confirm");
    reporter.check(
        confirmChild.process.get() != INVALID_HANDLE_VALUE,
        "start AskPass helper for host-key confirmation");
    const DWORD confirmExit = waitForExit(confirmChild.process.get());
    const std::string confirmOutput = collectOutput(confirmChild.output.get());
    reporter.check(
        confirmExit != 0 && confirmOutput.empty(),
        "host-key confirmation cannot consume or print the site password");

    std::cout << "[SUMMARY] failures=" << reporter.failures() << std::endl;
    return reporter.failures() == 0 ? 0 : 1;
}
