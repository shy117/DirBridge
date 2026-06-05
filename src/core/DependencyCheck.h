#ifndef XFOLDER_CORE_DEPENDENCYCHECK_H
#define XFOLDER_CORE_DEPENDENCYCHECK_H

#include <string>
#include <vector>

#include "core/CurlProtocolCheck.h"

struct DependencyCheckResult
{
    CurlProtocolCheckResult curl;
    bool jsonReady = false;
    bool loggingReady = false;
    bool siteStoreReady = false;
    std::string serializedSettings;
    std::string logPath;
    std::string siteConfigPath;
    std::vector<std::string> errors;
};

DependencyCheckResult checkDependencies(const std::string &configDirectory, const std::string &logDirectory);
std::string formatDependencyCheck(const DependencyCheckResult &result);
bool dependenciesReady(const DependencyCheckResult &result);

#endif // XFOLDER_CORE_DEPENDENCYCHECK_H
