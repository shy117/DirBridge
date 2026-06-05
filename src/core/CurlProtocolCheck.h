#ifndef XFOLDER_CORE_CURLPROTOCOLCHECK_H
#define XFOLDER_CORE_CURLPROTOCOLCHECK_H

#include <string>
#include <vector>

struct CurlProtocolCheckResult
{
    std::string version;
    std::vector<std::string> protocols;
    bool hasFtp = false;
    bool hasSftp = false;
};

CurlProtocolCheckResult checkCurlProtocols();
std::string formatCurlProtocolCheck(const CurlProtocolCheckResult &result);

#endif // XFOLDER_CORE_CURLPROTOCOLCHECK_H
