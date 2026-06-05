#include "core/DependencyCheck.h"

#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>
#include <spdlog/sinks/basic_file_sink.h>
#include <spdlog/spdlog.h>

DependencyCheckResult checkDependencies(const std::string &logDirectory)
{
    DependencyCheckResult result;
    result.curl = checkCurlProtocols();

    if (!result.curl.hasFtp || !result.curl.hasSftp)
    {
        result.errors.push_back("libcurl must support both ftp and sftp");
    }

    try
    {
        nlohmann::json settings = {
            {"app", "XFolder"},
            {"configVersion", 1},
            {"features", {"ftp", "sftp", "logging"}}
        };

        result.serializedSettings = settings.dump();
        const nlohmann::json parsed = nlohmann::json::parse(result.serializedSettings);
        result.jsonReady = parsed.value("app", "") == "XFolder";
        if (!result.jsonReady)
        {
            result.errors.push_back("nlohmann/json round trip failed");
        }
    }
    catch (const std::exception &error)
    {
        result.errors.push_back(std::string("nlohmann/json error: ") + error.what());
    }

    try
    {
        std::filesystem::create_directories(logDirectory);
        const std::filesystem::path logPath = std::filesystem::path(logDirectory) / "xfolder.log";
        auto logger = spdlog::basic_logger_mt("xfolder_dependency_check", logPath.string(), true);
        logger->set_level(spdlog::level::info);
        logger->info("XFolder dependency check: curl={}, json_ready={}", result.curl.version, result.jsonReady);
        logger->flush();
        spdlog::drop("xfolder_dependency_check");
        result.logPath = logPath.string();
        result.loggingReady = std::filesystem::exists(logPath);
        if (!result.loggingReady)
        {
            result.errors.push_back("spdlog did not create the log file");
        }
    }
    catch (const std::exception &error)
    {
        result.errors.push_back(std::string("spdlog error: ") + error.what());
    }

    return result;
}

std::string formatDependencyCheck(const DependencyCheckResult &result)
{
    std::ostringstream stream;
    stream << formatCurlProtocolCheck(result.curl) << '\n';
    stream << "nlohmann/json: " << (result.jsonReady ? "ready" : "failed") << '\n';
    stream << "spdlog: " << (result.loggingReady ? "ready" : "failed");
    if (!result.logPath.empty())
    {
        stream << " (" << result.logPath << ")";
    }
    stream << '\n';
    stream << "serialized settings: " << result.serializedSettings;

    if (!result.errors.empty())
    {
        stream << '\n' << "errors:";
        for (const std::string &error : result.errors)
        {
            stream << '\n' << "- " << error;
        }
    }

    return stream.str();
}

bool dependenciesReady(const DependencyCheckResult &result)
{
    return result.curl.hasFtp && result.curl.hasSftp && result.jsonReady && result.loggingReady && result.errors.empty();
}
