#ifndef XFOLDER_CORE_CURLPROTOCOLCHECK_H
#define XFOLDER_CORE_CURLPROTOCOLCHECK_H

#include <QString>
#include <QStringList>

struct CurlProtocolCheckResult
{
    QString version;
    QStringList protocols;
    bool hasFtp = false;
    bool hasSftp = false;
};

CurlProtocolCheckResult checkCurlProtocols();
QString formatCurlProtocolCheck(const CurlProtocolCheckResult &result);

#endif // XFOLDER_CORE_CURLPROTOCOLCHECK_H
