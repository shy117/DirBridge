#include "core/DependencyCheck.h"

#include "config/SiteStore.h"
#include "logging/AppLogger.h"

#include <filesystem>
#include <sstream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

DependencyCheckResult checkDependencies(const std::string &configDirectory, const std::string &logDirectory)
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
        auto logger = AppLogger::initialize(logDirectory);
        logger->info("XFolder dependency check: curl={}, json_ready={}", result.curl.version, result.jsonReady);
        logger->flush();
        result.logPath = AppLogger::logFilePath().string();
        result.loggingReady = std::filesystem::exists(AppLogger::logFilePath());
        if (!result.loggingReady)
        {
            result.errors.push_back("spdlog did not create the log file");
        }
    }
    catch (const std::exception &error)
    {
        result.errors.push_back(std::string("spdlog error: ") + error.what());
    }

    try
    {
        SiteStore store(std::filesystem::path(configDirectory) / "sites.json");
        std::vector<SiteProfile> sites = store.load();
        if (sites.empty())
        {
            sites.push_back(store.createDefaultSite());
            store.save(sites);
        }

        const std::vector<SiteProfile> reloaded = store.load();
        result.siteConfigPath = store.path().string();
        result.siteStoreReady = !reloaded.empty() && reloaded.front().protocol == RemoteProtocol::Sftp;
        if (!result.siteStoreReady)
        {
            result.errors.push_back("site config round trip failed");
        }
    }
    catch (const std::exception &error)
    {
        result.errors.push_back(std::string("site config error: ") + error.what());
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
    stream << "site config: " << (result.siteStoreReady ? "ready" : "failed");
    if (!result.siteConfigPath.empty())
    {
        stream << " (" << result.siteConfigPath << ")";
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
    return result.curl.hasFtp
        && result.curl.hasSftp
        && result.jsonReady
        && result.loggingReady
        && result.siteStoreReady
        && result.errors.empty();
}
