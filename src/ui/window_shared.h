#ifndef DIRBRIDGE_UI_WINDOW_SHARED_H
#define DIRBRIDGE_UI_WINDOW_SHARED_H

#include <cstdint>
#include <string>

#include <QIcon>
#include <QString>
#include <QStringList>

#include "config/SiteProfile.h"
#include "core/TransferJob.h"

class QComboBox;
class QWidget;

namespace window_shared
{
enum class SessionTreeItemType
{
    Group = 1,
    Site = 2,
    Recent = 3
};

inline constexpr int sessionItemTypeRole = Qt::UserRole;
inline constexpr int siteIndexRole = Qt::UserRole + 1;
inline constexpr int siteIdRole = Qt::UserRole + 2;
inline constexpr int remotePathRole = Qt::UserRole + 3;
inline constexpr int groupNameRole = Qt::UserRole + 4;

QString protocolText(RemoteProtocol protocol);
QIcon fluentIcon(const QString &name);
QString userFacingRemoteError(const QString &detail);
std::string makeSiteId(const SiteProfile &profile);
int findProtocolIndex(QComboBox *combo, RemoteProtocol protocol);
QString joinRemotePath(const QString &directory, const QString &name);
QString remoteBaseName(const QString &path);
bool isSameOrDescendantRemotePath(QString parent, QString candidate);
std::string makeTransferJobId(const QString &prefix);
std::int64_t currentEpochMillis();
QString transferDirectionText(TransferDirection direction);
QString transferStatusText(TransferStatus status);
QString transferSizeText(std::int64_t bytes);
QString transferSpeedText(double bytesPerSecond);
QString transferDurationText(std::int64_t milliseconds);
QString transferRemainingText(const TransferJob &job);
QString transferElapsedText(const TransferJob &job);
QString transferSizeProgressText(const TransferJob &job);
QString transferMessageText(const TransferJob &job);
QStringList ancestorRemoteDirectories(QString path);
QString normalizedRemotePath(QString path);
bool editSiteProfileDialog(QWidget *parent, SiteProfile &profile);
} // namespace window_shared

#endif // DIRBRIDGE_UI_WINDOW_SHARED_H
