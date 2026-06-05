#include "core/CurlProtocolCheck.h"

#include <algorithm>
#include <cctype>
#include <sstream>

#include <curl/curl.h>

CurlProtocolCheckResult checkCurlProtocols()
{
    CurlProtocolCheckResult result;

    const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    if (info == nullptr)
    {
        return result;
    }

    result.version = info->version;

    for (const char *const *protocol = info->protocols; protocol != nullptr && *protocol != nullptr; ++protocol)
    {
        std::string protocolName = *protocol;
        std::transform(protocolName.begin(), protocolName.end(), protocolName.begin(), [](unsigned char character) {
            return static_cast<char>(std::tolower(character));
        });
        result.protocols.push_back(protocolName);
    }

    result.hasFtp = std::find(result.protocols.begin(), result.protocols.end(), "ftp") != result.protocols.end();
    result.hasSftp = std::find(result.protocols.begin(), result.protocols.end(), "sftp") != result.protocols.end();

    return result;
}

std::string formatCurlProtocolCheck(const CurlProtocolCheckResult &result)
{
    std::ostringstream stream;
    stream << "libcurl " << (result.version.empty() ? "unknown" : result.version) << " protocols: ";
    for (std::size_t index = 0; index < result.protocols.size(); ++index)
    {
        if (index > 0)
        {
            stream << ", ";
        }
        stream << result.protocols.at(index);
    }
    return stream.str();
}
