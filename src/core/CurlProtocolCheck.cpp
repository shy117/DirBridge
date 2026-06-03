#include "core/CurlProtocolCheck.h"

#include <algorithm>

#include <curl/curl.h>

CurlProtocolCheckResult checkCurlProtocols()
{
    CurlProtocolCheckResult result;

    const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    if (info == nullptr)
    {
        return result;
    }

    result.version = QString::fromLatin1(info->version);

    for (const char *const *protocol = info->protocols; protocol != nullptr && *protocol != nullptr; ++protocol)
    {
        const QString protocolName = QString::fromLatin1(*protocol).toLower();
        result.protocols.append(protocolName);
    }

    result.hasFtp = result.protocols.contains("ftp");
    result.hasSftp = result.protocols.contains("sftp");

    return result;
}

QString formatCurlProtocolCheck(const CurlProtocolCheckResult &result)
{
    return QString("libcurl %1 protocols: %2")
        .arg(result.version.isEmpty() ? "unknown" : result.version)
        .arg(result.protocols.join(", "));
}
