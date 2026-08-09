#include "ui/MainWindow.h"

#include "logging/AppLogger.h"
#include "protocol/CurlRemoteFileSystem.h"
#include "core/TransferManager.h"
#include "ui/ExternalEditManager.h"
#include "ui/FilePanel.h"
#include "ui/window_shared.h"
#include "terminal/SshTerminalManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QCloseEvent>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QIcon>
#include <QInputDialog>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QMetaObject>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QProgressBar>
#include <QSplitter>
#include <QStatusBar>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTabBar>
#include <QThread>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <spdlog/spdlog.h>

namespace window_shared
{
QString protocolText(RemoteProtocol protocol)
{
    return QString::fromStdString(toString(protocol)).toUpper();
}

/**
 * @brief 从 Qt 资源中加载 Fluent UI SVG 图标。
 * @param name 不包含 _24_regular 后缀的图标基础名称。
 * @return 从 /icons/fluent 资源前缀加载得到的图标。
 */
QIcon fluentIcon(const QString &name)
{
    return QIcon(QString(":/icons/fluent/%1_24_regular.svg").arg(name));
}

/**
 * @brief 将底层远程后端错误转换为简洁的中文用户提示。
 * @param detail 后端错误详情，通常会写入日志用于诊断。
 * @return 适合用于消息框和状态标签的简短文本。
 */
QString userFacingRemoteError(const QString &detail)
{
    if (detail.contains("Connection timed out", Qt::CaseInsensitive))
    {
        return "连接超时，请检查主机地址、端口、网络或服务器状态。";
    }
    if (detail.contains("Could not resolve host", Qt::CaseInsensitive))
    {
        return "无法解析主机地址，请检查主机名是否正确。";
    }
    if (detail.contains("Couldn't connect", Qt::CaseInsensitive)
        || detail.contains("Failed to connect", Qt::CaseInsensitive))
    {
        return "无法连接服务器，请检查地址、端口和网络连通性。";
    }
    if (detail.contains("Authentication", Qt::CaseInsensitive)
        || detail.contains("Login denied", Qt::CaseInsensitive)
        || detail.contains("Access denied", Qt::CaseInsensitive))
    {
        return "认证失败，请检查用户名、密码或服务器权限。";
    }
    if (detail.contains("No such file", Qt::CaseInsensitive)
        || detail.contains("not found", Qt::CaseInsensitive))
    {
        return "远程目录不存在或当前账号没有访问权限。";
    }
    if (detail.trimmed().isEmpty())
    {
        return "远程操作失败。";
    }
    return "远程操作失败，请检查连接信息；详细错误已写入日志。";
}

std::string makeSiteId(const SiteProfile &profile)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    std::string host = profile.host.empty() ? "site" : profile.host;
    std::replace(host.begin(), host.end(), '.', '-');
    std::replace(host.begin(), host.end(), ':', '-');
    return toString(profile.protocol) + "-" + host + "-" + std::to_string(millis);
}

int findProtocolIndex(QComboBox *combo, RemoteProtocol protocol)
{
    return combo->findText(protocolText(protocol), Qt::MatchFixedString);
}

QString joinRemotePath(const QString &directory, const QString &name)
{
    QString path = directory.isEmpty() ? "/" : directory;
    if (!path.endsWith('/'))
    {
        path.append('/');
    }
    return path + name;
}

QString remoteBaseName(const QString &path)
{
    QString normalized = path;
    while (normalized.size() > 1 && normalized.endsWith('/'))
    {
        normalized.chop(1);
    }

    const int slashIndex = normalized.lastIndexOf('/');
    return slashIndex < 0 ? normalized : normalized.mid(slashIndex + 1);
}

bool isSameOrDescendantRemotePath(QString parent, QString candidate)
{
    while (parent.size() > 1 && parent.endsWith('/'))
    {
        parent.chop(1);
    }
    while (candidate.size() > 1 && candidate.endsWith('/'))
    {
        candidate.chop(1);
    }

    return candidate == parent || candidate.startsWith(parent + '/');
}

std::string makeTransferJobId(const QString &prefix)
{
    static std::atomic_uint counter = 0;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return prefix.toStdString() + "-" + std::to_string(millis) + "-" + std::to_string(++counter);
}

std::int64_t currentEpochMillis()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

QString transferDirectionText(TransferDirection direction)
{
    switch (direction)
    {
    case TransferDirection::Upload:
        return "上传";
    case TransferDirection::Download:
        return "下载";
    }

    return "上传";
}

QString transferStatusText(TransferStatus status)
{
    switch (status)
    {
    case TransferStatus::Preparing:
        return "准备中";
    case TransferStatus::Pending:
        return "等待中";
    case TransferStatus::Running:
        return "进行中";
    case TransferStatus::Completed:
        return "已完成";
    case TransferStatus::Failed:
        return "失败";
    case TransferStatus::Canceled:
        return "已取消";
    case TransferStatus::Canceling:
        return "取消中";
    }

    return "等待中";
}

QString transferSizeText(std::int64_t bytes)
{
    if (bytes < 0)
    {
        return "";
    }

    static const QStringList units = {"B", "KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < units.size() - 1)
    {
        value /= 1024.0;
        ++unitIndex;
    }

    if (unitIndex == 0)
    {
        return QString("%1 %2").arg(bytes).arg(units.at(unitIndex));
    }

    return QString("%1 %2").arg(value, 0, 'f', 1).arg(units.at(unitIndex));
}

QString transferSpeedText(double bytesPerSecond)
{
    if (bytesPerSecond <= 0.0)
    {
        return {};
    }

    static const QStringList units = {"B/s", "KB/s", "MB/s", "GB/s", "TB/s"};
    double value = bytesPerSecond;
    int unitIndex = 0;
    while (value >= 1024.0 && unitIndex < units.size() - 1)
    {
        value /= 1024.0;
        ++unitIndex;
    }

    if (unitIndex == 0)
    {
        return QString("%1 %2").arg(static_cast<qint64>(value)).arg(units.at(unitIndex));
    }
    return QString("%1 %2").arg(value, 0, 'f', 1).arg(units.at(unitIndex));
}

QString transferDurationText(std::int64_t milliseconds)
{
    if (milliseconds <= 0)
    {
        return {};
    }

    const qint64 totalSeconds = std::max<qint64>(1, milliseconds / 1000);
    const qint64 hours = totalSeconds / 3600;
    const qint64 minutes = (totalSeconds % 3600) / 60;
    const qint64 seconds = totalSeconds % 60;
    if (hours > 0)
    {
        return QString("%1 时 %2 分").arg(hours).arg(minutes, 2, 10, QChar('0'));
    }
    if (minutes > 0)
    {
        return QString("%1 分 %2 秒").arg(minutes).arg(seconds, 2, 10, QChar('0'));
    }
    return QString("%1 秒").arg(seconds);
}

QString transferRemainingText(const TransferJob &job)
{
    if (job.status != TransferStatus::Running
        || job.currentBytesPerSecond <= 0.0
        || job.totalBytes <= 0)
    {
        return {};
    }

    const std::int64_t remainingBytes = std::max<std::int64_t>(0, job.totalBytes - job.transferredBytes);
    if (remainingBytes <= 0)
    {
        return {};
    }
    return transferDurationText(static_cast<std::int64_t>((remainingBytes / job.currentBytesPerSecond) * 1000.0));
}

QString transferElapsedText(const TransferJob &job)
{
    if (job.startedAtMs <= 0)
    {
        return {};
    }
    const std::int64_t end = job.finishedAtMs > 0 ? job.finishedAtMs : currentEpochMillis();
    return transferDurationText(end - job.startedAtMs);
}

QString transferSizeProgressText(const TransferJob &job)
{
    if (job.totalBytes > 0)
    {
        return QString("%1/%2")
            .arg(transferSizeText(std::max<std::int64_t>(0, job.transferredBytes)))
            .arg(transferSizeText(job.totalBytes));
    }
    return transferSizeText(job.totalBytes);
}

QString transferMessageText(const TransferJob &job)
{
    const QString message = QString::fromStdString(job.errorMessage).trimmed();
    if (message.isEmpty())
    {
        return {};
    }
    if (message == "upload succeeded")
    {
        return "上传成功";
    }
    if (message == "download succeeded")
    {
        return "下载成功";
    }
    if (message == "transfer canceled")
    {
        return "传输已取消";
    }
    if (message == "remote session is not available")
    {
        return "远程会话不可用";
    }
    if (message == "unsupported transfer direction")
    {
        return "不支持的传输方向";
    }
    if (message.contains("failed", Qt::CaseInsensitive)
        || message.contains("not found", Qt::CaseInsensitive)
        || message.contains("cannot", Qt::CaseInsensitive)
        || message.contains("missing", Qt::CaseInsensitive))
    {
        return QString("传输失败：%1").arg(message);
    }

    return message;
}

QStringList ancestorRemoteDirectories(QString path)
{
    if (path.isEmpty())
    {
        path = "/";
    }
    if (!path.startsWith('/'))
    {
        path.prepend('/');
    }
    while (path.size() > 1 && path.endsWith('/'))
    {
        path.chop(1);
    }

    QStringList directories;
    directories << "/";
    QString accumulated;
    const QStringList parts = path.split('/', Qt::SkipEmptyParts);
    for (const QString &part : parts)
    {
        accumulated += "/" + part;
        directories << accumulated;
    }
    directories.removeDuplicates();
    return directories;
}

QString normalizedRemotePath(QString path)
{
    path = path.trimmed();
    if (path.isEmpty())
    {
        path = "/";
    }
    if (!path.startsWith('/'))
    {
        path.prepend('/');
    }
    return path;
}

/**
 * @brief 显示模态站点编辑器，并在确认后将修改结果写回配置对象。
 * @param parent 模态对话框的父部件。
 * @param profile 用户确认后要原地更新的站点配置。
 * @return 用户接受且配置有效时返回 true。
 */
bool editSiteProfileDialog(QWidget *parent, SiteProfile &profile)
{
    QDialog dialog(parent);
    dialog.setWindowTitle("站点属性");
    dialog.setObjectName("siteProfileDialog");

    auto *layout = new QVBoxLayout(&dialog);
    auto *form = new QFormLayout();

    auto *nameEdit = new QLineEdit(QString::fromStdString(profile.name), &dialog);
    nameEdit->setObjectName("siteNameEdit");
    auto *groupEdit = new QLineEdit(QString::fromStdString(profile.group), &dialog);
    groupEdit->setObjectName("siteGroupEdit");
    auto *protocolCombo = new QComboBox(&dialog);
    protocolCombo->setObjectName("siteProtocolCombo");
    protocolCombo->addItems({"SFTP", "FTP", "FTPS"});
    const int protocolIndex = protocolCombo->findText(protocolText(profile.protocol));
    if (protocolIndex >= 0)
    {
        protocolCombo->setCurrentIndex(protocolIndex);
    }
    auto *hostEdit = new QLineEdit(QString::fromStdString(profile.host), &dialog);
    hostEdit->setObjectName("siteHostEdit");
    auto *portEdit = new QLineEdit(QString::number(profile.port == 0 ? defaultPortForProtocol(profile.protocol) : profile.port), &dialog);
    portEdit->setObjectName("sitePortEdit");
    auto *userEdit = new QLineEdit(QString::fromStdString(profile.username), &dialog);
    userEdit->setObjectName("siteUserEdit");
    auto *passwordEdit = new QLineEdit(QString::fromStdString(profile.password), &dialog);
    passwordEdit->setObjectName("sitePasswordEdit");
    passwordEdit->setEchoMode(QLineEdit::Password);
    auto *remotePathEdit = new QLineEdit(QString::fromStdString(profile.defaultRemotePath.empty() ? "/" : profile.defaultRemotePath), &dialog);
    remotePathEdit->setObjectName("siteRemotePathEdit");
    auto *encodingEdit = new QLineEdit(QString::fromStdString(profile.encoding.empty() ? "UTF-8" : profile.encoding), &dialog);
    encodingEdit->setObjectName("siteEncodingEdit");

    form->addRow("名称", nameEdit);
    form->addRow("分组", groupEdit);
    form->addRow("协议", protocolCombo);
    form->addRow("主机", hostEdit);
    form->addRow("端口", portEdit);
    form->addRow("用户名", userEdit);
    form->addRow("密码", passwordEdit);
    form->addRow("默认路径", remotePathEdit);
    form->addRow("编码", encodingEdit);
    layout->addLayout(form);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
    buttons->button(QDialogButtonBox::Ok)->setText("确定");
    buttons->button(QDialogButtonBox::Cancel)->setText("取消");
    layout->addWidget(buttons);

    QObject::connect(protocolCombo, &QComboBox::currentTextChanged, &dialog, [portEdit](const QString &text) {
        bool ok = false;
        const int currentPort = portEdit->text().toInt(&ok);
        if (!ok || currentPort <= 0)
        {
            portEdit->setText(QString::number(defaultPortForProtocol(remoteProtocolFromString(text.toStdString()))));
        }
    });
    QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    QObject::connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
        if (hostEdit->text().trimmed().isEmpty())
        {
            QMessageBox::warning(&dialog, "站点信息不完整", "请输入主机地址。");
            return;
        }
        dialog.accept();
    });

    if (dialog.exec() != QDialog::Accepted)
    {
        return false;
    }

    profile.name = nameEdit->text().trimmed().toStdString();
    profile.group = groupEdit->text().trimmed().toStdString();
    profile.protocol = remoteProtocolFromString(protocolCombo->currentText().toStdString());
    profile.host = hostEdit->text().trimmed().toStdString();
    profile.port = static_cast<std::uint16_t>(portEdit->text().toUShort());
    if (profile.port == 0)
    {
        profile.port = defaultPortForProtocol(profile.protocol);
    }
    profile.username = userEdit->text().trimmed().toStdString();
    profile.password = passwordEdit->text().toStdString();
    profile.defaultRemotePath = normalizedRemotePath(remotePathEdit->text()).toStdString();
    profile.encoding = encodingEdit->text().trimmed().isEmpty() ? "UTF-8" : encodingEdit->text().trimmed().toStdString();
    if (profile.name.empty())
    {
        profile.name = protocolText(profile.protocol).toStdString() + " " + profile.host;
    }
    if (profile.id.empty())
    {
        profile.id = makeSiteId(profile);
    }
    return true;
}
} // namespace window_shared

using namespace window_shared;

namespace
{
QString externalEditStateLabel(ExternalEditState state)
{
    switch (state)
    {
    case ExternalEditState::Downloading:
        return "正在准备编辑";
    case ExternalEditState::DownloadFailed:
        return "下载失败";
    case ExternalEditState::OpenFailed:
        return "无法打开编辑器";
    case ExternalEditState::EditingClean:
        return "正在编辑（已同步）";
    case ExternalEditState::PendingUpload:
        return "等待同步";
    case ExternalEditState::Uploading:
        return "正在同步";
    case ExternalEditState::UploadFailed:
        return "同步失败";
    case ExternalEditState::Conflict:
        return "远程文件已变化";
    case ExternalEditState::Closed:
        return "已停止编辑";
    }
    return {};
}
}

MainWindow::MainWindow(const DependencyCheckResult &dependencyCheck, QWidget *parent)
    : QMainWindow(parent)
    , m_siteStore(dependencyCheck.siteConfigPath.empty() ? std::filesystem::path("config") / "sites.json" : dependencyCheck.siteConfigPath)
    , m_settingsStore(m_siteStore.path().parent_path() / "settings.json")
{
    setWindowTitle("DirBridge");
    setWindowIcon(QIcon(":/icons/app/dirbridge.ico"));
    resize(1380, 820);

    loadSites();
    loadSettings();
    setupCentralWorkspace(dependencyCheck);
    setupSshTerminalManager();
    ExternalEditManager::Callbacks externalEditCallbacks;
    externalEditCallbacks.resolveFileSystem = [this](const QString &sessionId) {
        RemoteSession *session = remoteSessionById(sessionId.toStdString());
        return session != nullptr && session->connected ? session->fileSystem : std::shared_ptr<RemoteFileSystem>();
    };
    externalEditCallbacks.canStartRemoteOperation = [this](const QString &sessionId) {
        RemoteSession *session = remoteSessionById(sessionId.toStdString());
        return !m_closePending
            && session != nullptr
            && session->connected
            && session->fileSystem != nullptr
            && !hasRunningTransferForSession(session->id);
    };
    externalEditCallbacks.beginBackgroundTask = [this]() {
        beginBackgroundTask();
    };
    externalEditCallbacks.finishBackgroundTask = [this]() {
        finishBackgroundTask();
    };
    externalEditCallbacks.showWarning = [this](const QString &title, const QString &message) {
        showWarningMessage(title, message);
    };
    externalEditCallbacks.stateChanged = [this](const QString &sessionId, const QString &remotePath, ExternalEditState state) {
        appendLog("INFO", QString("远程编辑状态：[%1] %2 — %3")
            .arg(sessionId, remotePath, externalEditStateLabel(state)));
    };
    externalEditCallbacks.remoteFileSynchronized = [this](const QString &sessionId, const QString &) {
        RemoteSession *session = remoteSessionById(sessionId.toStdString());
        if (session != nullptr && session->connected && !session->currentPath.isEmpty())
        {
            loadRemotePath(*session, session->currentPath, false);
        }
    };
    const QString editorCacheRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    m_externalEditManager = std::make_unique<ExternalEditManager>(
        std::filesystem::path((editorCacheRoot.isEmpty() ? QDir::homePath() : editorCacheRoot).toStdWString()) / "editor-cache",
        std::move(externalEditCallbacks),
        this);
    setupMenuBar();
    setupToolBar();
    setupQuickConnectBar();
    populateSessionManager();
    updateRemoteConnectionActions();

    appendLog("INFO", "DirBridge UI started");
    appendLog("INFO", QString("site config: %1").arg(QString::fromStdString(m_siteStore.path().string())));
    appendLog("INFO", QString("user settings: %1").arg(QString::fromStdString(m_settingsStore.path().string())));
    appendLog("INFO", QString("libcurl ready=%1, JSON ready=%2, logging ready=%3")
        .arg(dependencyCheck.curl.hasFtp && dependencyCheck.curl.hasSftp ? "yes" : "no")
        .arg(dependencyCheck.jsonReady ? "yes" : "no")
        .arg(dependencyCheck.loggingReady && dependencyCheck.siteStoreReady ? "yes" : "no"));

    const bool startupReady = dependencyCheck.curl.hasFtp
        && dependencyCheck.curl.hasSftp
        && dependencyCheck.jsonReady
        && dependencyCheck.loggingReady
        && dependencyCheck.siteStoreReady;
    statusBar()->showMessage(startupReady ? "就绪" : "启动检查发现异常，请查看日志。");
}

MainWindow::~MainWindow() = default;

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_closeReady)
    {
        event->accept();
        return;
    }

    if (!m_closePending)
    {
        m_closePending = true;
        setEnabled(false);
        cancelActiveTransfersForClose();
        if (m_sshTerminalManager != nullptr)
        {
            m_sshTerminalManager->beginShutdown();
        }
        statusBar()->showMessage("正在停止后台任务后退出…");
    }

    if (m_activeBackgroundTaskCount > 0
        || (m_sshTerminalManager != nullptr
            && m_sshTerminalManager->sessionCount() > 0))
    {
        event->ignore();
        return;
    }

    m_closeReady = true;
    event->accept();
}

void MainWindow::beginBackgroundTask()
{
    ++m_activeBackgroundTaskCount;
}

void MainWindow::finishBackgroundTask()
{
    if (m_activeBackgroundTaskCount <= 0)
    {
        return;
    }

    --m_activeBackgroundTaskCount;
    if (m_closePending && m_activeBackgroundTaskCount == 0
        && (m_sshTerminalManager == nullptr
            || m_sshTerminalManager->sessionCount() == 0))
    {
        QTimer::singleShot(0, this, [this]() {
            close();
        });
    }
}

void MainWindow::cancelActiveTransfersForClose()
{
    for (const auto &[jobId, cancelFlag] : m_transferCancelFlags)
    {
        Q_UNUSED(jobId);
        if (cancelFlag != nullptr)
        {
            cancelFlag->store(true);
        }
    }

    for (const TransferJob &job : m_transferQueue.jobs())
    {
        if (job.status == TransferStatus::Preparing
            || job.status == TransferStatus::Pending
            || job.status == TransferStatus::Running)
        {
            m_transferQueue.cancel(job.id, "应用正在退出");
        }
    }
    refreshTransferTable();
}

void MainWindow::setRemoteFileSystemForTesting(std::unique_ptr<RemoteFileSystem> remoteFileSystem)
{
    if (remoteFileSystem == nullptr)
    {
        return;
    }

    m_testingRemoteFileSystem = std::move(remoteFileSystem);
}

void MainWindow::uploadLocalFileForTesting(const QString &localPath)
{
    uploadLocalFile(localPath);
}

void MainWindow::uploadLocalPathForTesting(const QString &localPath)
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr)
    {
        uploadLocalPath(*session, localPath);
    }
}

void MainWindow::saveSiteForTesting(const SiteProfile &profile)
{
    const auto existing = std::find_if(m_sites.begin(), m_sites.end(), [&profile](const SiteProfile &site) {
        return site.id == profile.id;
    });
    if (existing == m_sites.end())
    {
        m_sites.push_back(profile);
    }
    else
    {
        *existing = profile;
    }
    ensureSiteGroupStored(profile.group);
    saveSites();
}

bool MainWindow::removeSiteForTesting(const std::string &siteId)
{
    const auto oldSize = m_sites.size();
    m_sites.erase(std::remove_if(m_sites.begin(), m_sites.end(), [&siteId](const SiteProfile &site) {
        return site.id == siteId;
    }), m_sites.end());
    if (m_sites.size() == oldSize)
    {
        return false;
    }
    saveSites();
    return true;
}

bool MainWindow::createSiteGroupForTesting(const QString &groupName)
{
    return createSiteGroup(groupName);
}

bool MainWindow::deleteSiteGroupForTesting(const QString &groupName)
{
    return deleteSiteGroup(groupName);
}

bool MainWindow::removeRecentSessionForTesting(const std::string &siteId)
{
    const auto oldSize = m_settings.recentSessions.size();
    removeRecentSession(siteId);
    return m_settings.recentSessions.size() != oldSize;
}

void MainWindow::clearRecentSessionsForTesting()
{
    clearRecentSessions();
}

bool MainWindow::renameSiteGroupForTesting(const QString &oldGroup, const QString &newGroup)
{
    return renameSiteGroup(oldGroup, newGroup);
}

void MainWindow::downloadRemoteFileForTesting(const QString &remotePath)
{
    downloadRemoteFile(remotePath);
}

void MainWindow::downloadRemotePathForTesting(const QString &remotePath)
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr)
    {
        downloadRemotePath(*session, remotePath);
    }
}

void MainWindow::removeRemotePathForTesting(const QString &path)
{
    removeRemotePath(path);
}

void MainWindow::setRemotePermissionsForTesting(const QString &path, int mode, bool recursive)
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr)
    {
        setRemotePermissions(*session, path, mode, recursive);
    }
}

void MainWindow::moveRemotePathsForTesting(const QStringList &sourcePaths, const QString &targetDirectory)
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr)
    {
        moveRemotePaths(*session, sourcePaths, targetDirectory);
    }
}

void MainWindow::setDialogsSuppressedForTesting(bool suppressed)
{
    m_dialogsSuppressedForTesting = suppressed;
}

void MainWindow::setLocalPathForTesting(const QString &path)
{
    if (m_localPanel != nullptr)
    {
        m_localPanel->setLocalPathForTesting(path);
    }
}

void MainWindow::editRemoteFileForTesting(const QString &remotePath)
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr && m_externalEditManager != nullptr)
    {
        m_externalEditManager->openRemoteFile(session->id, remotePath);
    }
}

void MainWindow::closeRemoteEditForTesting(const QString &remotePath)
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr && m_externalEditManager != nullptr)
    {
        m_externalEditManager->closeDocument(session->id, remotePath);
    }
}

void MainWindow::setCurrentRemoteFileTreeVisibleForTesting(bool visible)
{
    setFileTreeVisibilityForSession(currentRemoteSession(), visible);
}

void MainWindow::setExternalEditorLauncherForTesting(std::function<bool(const QString &)> launcher)
{
    if (m_externalEditManager != nullptr)
    {
        m_externalEditManager->setExternalFileLauncher(std::move(launcher));
    }
}

void MainWindow::showWarningMessage(const QString &title, const QString &message)
{
    if (m_dialogsSuppressedForTesting)
    {
        appendLog("WARN", QString("%1：%2").arg(title, message));
        return;
    }

    QMessageBox::warning(this, title, message);
}

void MainWindow::showCriticalMessage(const QString &title, const QString &message)
{
    if (m_dialogsSuppressedForTesting)
    {
        appendLog("ERROR", QString("%1：%2").arg(title, message));
        return;
    }

    QMessageBox::critical(this, title, message);
}

void MainWindow::loadSites()
{
    try
    {
        m_sites = m_siteStore.load();
        m_siteGroups = m_siteStore.loadGroups();
        bool migratedGroups = false;
        for (const SiteProfile &site : m_sites)
        {
            if (site.group.empty())
            {
                continue;
            }
            if (std::find(m_siteGroups.begin(), m_siteGroups.end(), site.group) == m_siteGroups.end())
            {
                m_siteGroups.push_back(site.group);
                migratedGroups = true;
            }
        }
        if (migratedGroups)
        {
            m_siteStore.save(m_sites, m_siteGroups);
        }
    }
    catch (const std::exception &error)
    {
        m_sites.clear();
        m_siteGroups.clear();
        appendLog("ERROR", QString("加载站点配置失败：%1").arg(error.what()));
    }
}

void MainWindow::saveSites()
{
    try
    {
        m_siteStore.save(m_sites, m_siteGroups);
        populateSessionManager();
    }
    catch (const std::exception &error)
    {
        appendLog("ERROR", QString("保存站点配置失败：%1").arg(error.what()));
        showCriticalMessage("保存站点配置失败", error.what());
    }
}

bool MainWindow::ensureSiteGroupStored(const std::string &groupName)
{
    const QString normalizedGroupName = QString::fromStdString(groupName).trimmed();
    if (normalizedGroupName.isEmpty() || normalizedGroupName == QString("未分组"))
    {
        return false;
    }

    const bool exists = std::any_of(m_siteGroups.begin(), m_siteGroups.end(), [&normalizedGroupName](const std::string &storedGroup) {
        return QString::fromStdString(storedGroup).trimmed().compare(normalizedGroupName, Qt::CaseInsensitive) == 0;
    });
    if (exists)
    {
        return false;
    }

    m_siteGroups.push_back(normalizedGroupName.toStdString());
    return true;
}

void MainWindow::loadSettings()
{
    try
    {
        m_settings = m_settingsStore.load();
    }
    catch (const std::exception &error)
    {
        m_settings = {};
        appendLog("ERROR", QString("加载用户设置失败：%1").arg(error.what()));
    }
}

void MainWindow::saveSettings()
{
    try
    {
        m_settingsStore.save(m_settings);
        populateSessionManager();
    }
    catch (const std::exception &error)
    {
        appendLog("ERROR", QString("保存用户设置失败：%1").arg(error.what()));
        showCriticalMessage("保存用户设置失败", error.what());
    }
}

bool MainWindow::hasConnectingRemoteSession() const
{
    return std::any_of(m_remoteSessions.begin(), m_remoteSessions.end(), [](const std::unique_ptr<RemoteSession> &session) {
        return session != nullptr && session->connecting;
    });
}

void MainWindow::appendLog(const QString &level, const QString &message)
{
    if (m_logView != nullptr)
    {
        auto *item = new QTreeWidgetItem(m_logView, {
            QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss"),
            level,
            message
        });
        m_logView->addTopLevelItem(item);
        m_logView->scrollToItem(item);
    }

    auto logger = AppLogger::get();
    if (logger == nullptr)
    {
        return;
    }

    const std::string text = message.toStdString();
    if (level == "ERROR")
    {
        logger->error(text);
    }
    else if (level == "WARN")
    {
        logger->warn(text);
    }
    else
    {
        logger->info(text);
    }
    logger->flush();
}
