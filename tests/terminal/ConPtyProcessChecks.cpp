#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>

#include "terminal/ConPtyProcess.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

int main()
{
    int argumentCount = 0;
    wchar_t **arguments = CommandLineToArgvW(
        GetCommandLineW(),
        &argumentCount);
    if (arguments == nullptr || argumentCount != 2)
    {
        if (arguments != nullptr)
        {
            LocalFree(arguments);
        }
        std::cerr << "usage: ConPtyProcessChecks <child>\n";
        return 1;
    }

    dirbridge::terminal::OpenSshLaunchSpec spec;
    spec.executablePath = std::filesystem::path(arguments[1]);
    LocalFree(arguments);
    spec.workingDirectory = std::filesystem::current_path();

    dirbridge::terminal::ConPtyProcess process;
    if (!process.start(spec, {}, 80, 25))
    {
        std::cerr << "[FAIL] " << process.error() << '\n';
        return 1;
    }
    std::uint32_t exitCode = 0;
    if (!process.wait(std::chrono::seconds(10), exitCode))
    {
        std::cerr << "[FAIL] " << process.error() << '\n';
        process.terminate(1);
        return 1;
    }
    const auto output = process.takeOutput();
    const std::string marker = "DIRBRIDGE_CONPTY_PRODUCTION_OK";
    const bool found = std::search(
        output.begin(),
        output.end(),
        marker.begin(),
        marker.end()) != output.end();
    std::cout << (found ? "[PASS] " : "[FAIL] ")
              << "production ConPTY process output\n";
    std::cout << "[SUMMARY] exit=" << exitCode
              << " output_bytes=" << output.size() << '\n';
    return found && exitCode == 0 ? 0 : 1;
}
