#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cwchar>
#include <string>
#include <string_view>
#include <vector>

#include "ConPtyTestProtocol.h"

namespace {

using dirbridge::terminal::test::kOutputBlockCount;
using dirbridge::terminal::test::makeOutputBlock;

bool writeBytes(const void* data, std::size_t size)
{
    const auto* cursor = static_cast<const std::uint8_t*>(data);
    HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    while (size > 0) {
        const DWORD chunk = size > MAXDWORD
            ? MAXDWORD
            : static_cast<DWORD>(size);
        DWORD written = 0;
        if (!WriteFile(output, cursor, chunk, &written, nullptr)
            || written == 0) {
            return false;
        }
        cursor += written;
        size -= written;
    }
    return true;
}

bool writeText(std::string_view text)
{
    return writeBytes(text.data(), text.size());
}

bool writeUtf8(std::wstring_view text)
{
    if (text.empty()) {
        return true;
    }

    const int inputSize = static_cast<int>(text.size());
    const int outputSize = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        text.data(),
        inputSize,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (outputSize <= 0) {
        return false;
    }

    std::string output(static_cast<std::size_t>(outputSize), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            text.data(),
            inputSize,
            output.data(),
            outputSize,
            nullptr,
            nullptr)
        != outputSize) {
        return false;
    }
    return writeBytes(output.data(), output.size());
}

bool configureUtf8Console()
{
    HANDLE input = CreateFileW(
        L"CONIN$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        return false;
    }

    HANDLE output = CreateFileW(
        L"CONOUT$",
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr);
    if (output == INVALID_HANDLE_VALUE) {
        CloseHandle(input);
        return false;
    }

    if (!SetStdHandle(STD_INPUT_HANDLE, input)
        || !SetStdHandle(STD_OUTPUT_HANDLE, output)
        || !SetStdHandle(STD_ERROR_HANDLE, output)) {
        CloseHandle(input);
        CloseHandle(output);
        return false;
    }

    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);
    return true;
}

void configureRawInput()
{
    HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(input, &mode)) {
        mode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
        mode |= ENABLE_PROCESSED_INPUT;
        SetConsoleMode(input, mode);
    }
}

int runCooperativeClose()
{
    configureRawInput();
    if (!writeText("READY COOPERATIVE\r\n")) {
        return 2;
    }

    std::array<wchar_t, 32> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadConsoleW(
                GetStdHandle(STD_INPUT_HANDLE),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                nullptr)) {
            return 3;
        }
        for (DWORD index = 0; index < read; ++index) {
            if (buffer[index] == L'x') {
                writeText("COOPERATIVE EXIT\r\n");
                return 0;
            }
        }
    }
}

int runStubbornClose()
{
    if (!writeText("READY STUBBORN\r\n")) {
        return 2;
    }
    for (;;) {
        Sleep(INFINITE);
    }
}

int runUtf8Echo()
{
    configureRawInput();
    if (!writeText("READY ECHO\r\n")) {
        return 2;
    }

    std::wstring received;
    std::array<wchar_t, 256> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadConsoleW(
                GetStdHandle(STD_INPUT_HANDLE),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                nullptr)) {
            return 3;
        }
        if (read == 0) {
            continue;
        }
        received.append(buffer.data(), read);
        if (!writeUtf8(std::wstring_view(buffer.data(), read))) {
            return 4;
        }
        if (received.find(L"__DIRBRIDGE_ECHO_END__")
            != std::wstring::npos) {
            return 0;
        }
    }
}

int runOutput64MiB()
{
    constexpr std::size_t kBlocksPerWrite = 16;
    std::array<
        std::uint8_t,
        dirbridge::terminal::test::kOutputBlockSize
            * kBlocksPerWrite> output{};
    for (std::size_t firstSequence = 0;
         firstSequence < kOutputBlockCount;
         firstSequence += kBlocksPerWrite) {
        for (std::size_t blockIndex = 0;
             blockIndex < kBlocksPerWrite;
             ++blockIndex) {
            const auto block =
                makeOutputBlock(firstSequence + blockIndex);
            std::copy(
                block.begin(),
                block.end(),
                output.begin()
                    + blockIndex
                        * dirbridge::terminal::test::kOutputBlockSize);
        }
        if (!writeBytes(output.data(), output.size())) {
            return 2;
        }
    }
    return 0;
}

int runSizeReport()
{
    configureRawInput();
    if (!writeText("READY SIZE\r\n")) {
        return 2;
    }

    std::array<wchar_t, 32> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!ReadConsoleW(
                GetStdHandle(STD_INPUT_HANDLE),
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                nullptr)) {
            return 3;
        }
        for (DWORD index = 0; index < read; ++index) {
            if (buffer[index] == L'q') {
                return 0;
            }
            if (buffer[index] != L's') {
                continue;
            }

            CONSOLE_SCREEN_BUFFER_INFO info{};
            if (!GetConsoleScreenBufferInfo(
                    GetStdHandle(STD_OUTPUT_HANDLE),
                    &info)) {
                return 4;
            }

            char line[64]{};
            const int length = std::snprintf(
                line,
                sizeof(line),
                "SIZE %d %d\r\n",
                static_cast<int>(info.dwSize.X),
                static_cast<int>(info.srWindow.Bottom
                    - info.srWindow.Top + 1));
            if (length <= 0
                || !writeBytes(line, static_cast<std::size_t>(length))) {
                return 5;
            }
        }
    }
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

int runResidentLeaf()
{
    for (;;) {
        Sleep(INFINITE);
    }
}

int runSpawnResident()
{
    std::vector<wchar_t> modulePath(32768);
    const DWORD length = GetModuleFileNameW(
        nullptr,
        modulePath.data(),
        static_cast<DWORD>(modulePath.size()));
    if (length == 0 || length >= modulePath.size()) {
        return 2;
    }

    const std::wstring executable(modulePath.data(), length);
    std::wstring commandLine =
        quoteArgument(executable) + L" --mode resident-leaf";
    std::vector<wchar_t> mutableCommand(
        commandLine.begin(),
        commandLine.end());
    mutableCommand.push_back(L'\0');

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(
            executable.c_str(),
            mutableCommand.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_UNICODE_ENVIRONMENT,
            nullptr,
            nullptr,
            &startup,
            &process)) {
        return 3;
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    char line[64]{};
    const int lineLength = std::snprintf(
        line,
        sizeof(line),
        "SPAWNED %lu\r\n",
        static_cast<unsigned long>(process.dwProcessId));
    if (lineLength <= 0
        || !writeBytes(line, static_cast<std::size_t>(lineLength))) {
        return 4;
    }

    for (;;) {
        Sleep(INFINITE);
    }
}

std::wstring argumentValue(
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

} // namespace

int run(int argumentCount, wchar_t** arguments)
{
    if (!configureUtf8Console()) {
        return 65;
    }
    const std::wstring mode =
        argumentValue(argumentCount, arguments, L"--mode");

    if (mode == L"cooperative-close") {
        return runCooperativeClose();
    }
    if (mode == L"stubborn-close") {
        return runStubbornClose();
    }
    if (mode == L"utf8-echo") {
        return runUtf8Echo();
    }
    if (mode == L"output-64m") {
        return runOutput64MiB();
    }
    if (mode == L"size-report") {
        return runSizeReport();
    }
    if (mode == L"spawn-resident") {
        return runSpawnResident();
    }
    if (mode == L"resident-leaf") {
        return runResidentLeaf();
    }

    writeText("Unknown --mode value.\r\n");
    return 64;
}

int main()
{
    int argumentCount = 0;
    wchar_t** arguments =
        CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments) {
        return 64;
    }
    const int result = run(argumentCount, arguments);
    LocalFree(arguments);
    return result;
}
