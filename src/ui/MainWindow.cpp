#include "ui/MainWindow.h"

#include "logging/AppLogger.h"
#include "protocol/CurlRemoteFileSystem.h"
#include "core/TransferManager.h"
#include "ui/FilePanel.h"

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
#include <QTabWidget>
#include <QTabBar>
#include <QThread>
#include <QToolBar>
#include <QTreeWidget>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWidget>

#include <spdlog/spdlog.h>

namespace
{
QString protocolText(RemoteProtocol protocol)
{
    return QString::fromStdString(toString(protocol)).toUpper();
}

enum class SessionTreeItemType
{
    Group = 1,
    Site = 2,
    Recent = 3
};

/**
 * @brief Creates a Fluent UI SVG icon from the Qt resource bundle.
 * @param name Fluent icon base name without the _24_regular suffix.
 * @return Icon loaded from the /icons/fluent resource prefix.
 */
QIcon fluentIcon(const QString &name)
{
    return QIcon(QString(":/icons/fluent/%1_24_regular.svg").arg(name));
}

/**
 * @brief Converts low-level remote backend errors into concise user-facing Chinese text.
 * @param detail Backend error detail, usually logged for diagnostics.
 * @return Short text suitable for message boxes and status labels.
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

constexpr int sessionItemTypeRole = Qt::UserRole;
constexpr int siteIndexRole = Qt::UserRole + 1;
constexpr int siteIdRole = Qt::UserRole + 2;
constexpr int remotePathRole = Qt::UserRole + 3;
constexpr int groupNameRole = Qt::UserRole + 4;

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
 * @brief Shows the modal site editor and writes edited values back to the profile.
 * @param parent Parent widget for the modal dialog.
 * @param profile Site profile to edit in-place when accepted.
 * @return true when the user accepted a valid profile.
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
}

struct MainWindow::RemoteConnectionResult
{
    SiteProfile profile;
    QString requestedPath;
    QString finalMessage;
    QString detail;
    QStringList knownDirectories;
    std::vector<FileItem> items;
    std::unique_ptr<RemoteFileSystem> fileSystem;
    bool canceled = false;
    bool connected = false;
    bool loaded = false;
};

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

    statusBar()->showMessage(
        QString("libcurl ready=%1, JSON ready=%2, logging ready=%3")
            .arg(dependencyCheck.curl.hasFtp && dependencyCheck.curl.hasSftp ? "yes" : "no")
            .arg(dependencyCheck.jsonReady ? "yes" : "no")
            .arg(dependencyCheck.loggingReady && dependencyCheck.siteStoreReady ? "yes" : "no"));
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
    }
    catch (const std::exception &error)
    {
        m_sites.clear();
        appendLog("ERROR", QString("加载站点配置失败：%1").arg(error.what()));
    }
}

void MainWindow::saveSites()
{
    try
    {
        m_siteStore.save(m_sites);
        populateSessionManager();
    }
    catch (const std::exception &error)
    {
        appendLog("ERROR", QString("保存站点配置失败：%1").arg(error.what()));
        showCriticalMessage("保存站点配置失败", error.what());
    }
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

void MainWindow::setupMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu("文件(&F)");
    fileMenu->addAction(fluentIcon("add"), "新建站点", this, [this]() {
        editSiteAtIndex(-1);
    });

    QMenu *viewMenu = menuBar()->addMenu("查看(&V)");
    QAction *sessionManagerAction = nullptr;
    if (m_sessionDock != nullptr)
    {
        sessionManagerAction = m_sessionDock->toggleViewAction();
        sessionManagerAction->setText("会话管理器");
        sessionManagerAction->setIcon(fluentIcon("more_horizontal"));
        sessionManagerAction->setShortcut(QKeySequence("Ctrl+1"));
        viewMenu->addAction(sessionManagerAction);
    }

    auto *fileTreeAction = viewMenu->addAction(fluentIcon("folder_add"), "文件树");
    fileTreeAction->setCheckable(true);
    fileTreeAction->setChecked(true);
    connect(fileTreeAction, &QAction::toggled, this, [this](bool visible) {
        if (m_localPanel != nullptr)
        {
            m_localPanel->setFileTreeVisible(visible);
        }
        for (const auto &session : m_remoteSessions)
        {
            if (session->panel != nullptr)
            {
                session->panel->setFileTreeVisible(visible);
            }
        }
    });

    m_disconnectAction = new QAction(fluentIcon("dismiss_circle"), "断开", this);
    connect(m_disconnectAction, &QAction::triggered, this, &MainWindow::disconnectRemote);
    m_disconnectAction->setEnabled(false);
    m_refreshAction = new QAction(fluentIcon("arrow_sync"), "刷新远程", this);
    connect(m_refreshAction, &QAction::triggered, this, &MainWindow::refreshRemote);

    menuBar()->addMenu("帮助(&H)")->addAction(fluentIcon("info"), "关于 DirBridge", this, &MainWindow::showAboutDialog);
}

void MainWindow::showAboutDialog()
{
    QDialog dialog(this);
    dialog.setWindowTitle("关于 DirBridge");
    dialog.setObjectName("aboutDialog");

    auto *layout = new QVBoxLayout(&dialog);
    auto *titleLabel = new QLabel("<b>DirBridge</b>", &dialog);
    auto *detailLabel = new QLabel(
        QString("版本：%1<br>"
                "许可证：Apache-2.0<br>"
                "GitHub：<a href=\"https://github.com/shy117/DirBridge\">https://github.com/shy117/DirBridge</a><br>"
                "作者：ShyHiker")
            .arg(QApplication::applicationVersion()),
        &dialog);
    detailLabel->setTextFormat(Qt::RichText);
    detailLabel->setTextInteractionFlags(Qt::TextBrowserInteraction);
    detailLabel->setOpenExternalLinks(true);
    layout->addWidget(titleLabel);
    layout->addWidget(detailLabel);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close, &dialog);
    buttons->button(QDialogButtonBox::Close)->setText("关闭");
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    dialog.exec();
}

void MainWindow::setupToolBar()
{
}

void MainWindow::setupQuickConnectBar()
{
    QToolBar *quickBar = addToolBar("快速连接");
    quickBar->setMovable(false);

    m_protocolCombo = new QComboBox(quickBar);
    m_protocolCombo->setObjectName("quickProtocolCombo");
    m_protocolCombo->addItems({"SFTP", "FTP", "FTPS"});

    m_hostEdit = new QLineEdit(quickBar);
    m_hostEdit->setObjectName("quickHostEdit");
    m_hostEdit->setPlaceholderText("主机地址");

    m_portEdit = new QLineEdit(quickBar);
    m_portEdit->setObjectName("quickPortEdit");
    m_portEdit->setPlaceholderText("端口");
    m_portEdit->setToolTip("远程服务端口，例如 SFTP 默认 22，FTP 默认 21。");
    m_portEdit->setMinimumWidth(84);
    m_portEdit->setText(QString::number(defaultPortForProtocol(RemoteProtocol::Sftp)));

    m_userEdit = new QLineEdit(quickBar);
    m_userEdit->setObjectName("quickUserEdit");
    m_userEdit->setPlaceholderText("用户名");

    m_passwordEdit = new QLineEdit(quickBar);
    m_passwordEdit->setObjectName("quickPasswordEdit");
    m_passwordEdit->setPlaceholderText("密码");
    m_passwordEdit->setEchoMode(QLineEdit::Password);

    m_remotePathEdit = new QLineEdit(quickBar);
    m_remotePathEdit->setObjectName("quickRemotePathEdit");
    m_remotePathEdit->setPlaceholderText("远程路径");
    m_remotePathEdit->setToolTip("连接后打开的远程目录。SFTP 可使用 /home/testuser/remote_test；FTP 测试环境使用 /remote_test。");
    m_remotePathEdit->setMinimumWidth(180);
    m_remotePathEdit->setText("/");

    m_connectButton = new QPushButton("连接", quickBar);
    m_connectButton->setObjectName("quickConnectButton");
    m_connectButton->setIcon(fluentIcon("checkmark_circle"));
    m_saveSiteButton = new QPushButton("保存为站点", quickBar);
    m_saveSiteButton->setObjectName("quickSaveSiteButton");
    m_saveSiteButton->setIcon(fluentIcon("folder_add"));

    quickBar->addWidget(new QLabel("协议", quickBar));
    quickBar->addWidget(m_protocolCombo);
    quickBar->addWidget(new QLabel("主机", quickBar));
    quickBar->addWidget(m_hostEdit);
    quickBar->addWidget(new QLabel("端口", quickBar));
    quickBar->addWidget(m_portEdit);
    quickBar->addWidget(new QLabel("用户", quickBar));
    quickBar->addWidget(m_userEdit);
    quickBar->addWidget(new QLabel("密码", quickBar));
    quickBar->addWidget(m_passwordEdit);
    quickBar->addWidget(new QLabel("路径", quickBar));
    quickBar->addWidget(m_remotePathEdit);
    quickBar->addWidget(m_connectButton);
    quickBar->addWidget(m_saveSiteButton);

    connect(m_protocolCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        const RemoteProtocol protocol = remoteProtocolFromString(text.toStdString());
        m_portEdit->setText(QString::number(defaultPortForProtocol(protocol)));
    });
    connect(m_connectButton, &QPushButton::clicked, this, [this]() {
        RemoteSession *session = currentRemoteSession();
        if (session != nullptr && session->connecting)
        {
            cancelRemoteConnection(*session);
            return;
        }
        connectQuickProfile(false);
    });
    connect(m_saveSiteButton, &QPushButton::clicked, this, [this]() {
        SiteProfile profile = profileFromQuickConnect();
        if (profile.host.empty())
        {
            showWarningMessage("站点信息不完整", "请输入主机地址。");
            return;
        }
        if (!editSiteProfileDialog(this, profile))
        {
            return;
        }
        m_sites.push_back(profile);
        saveSites();
        appendLog("INFO", QString("已新建站点：%1").arg(siteDisplayName(profile)));
    });
}

void MainWindow::setupCentralWorkspace(const DependencyCheckResult &dependencyCheck)
{
    m_sessionDock = new QDockWidget("Session Manager", this);
    m_sessionDock->setObjectName("SessionManagerDock");
    m_sessionDock->setWidget(createSessionManager());
    addDockWidget(Qt::LeftDockWidgetArea, m_sessionDock);

    auto *verticalSplitter = new QSplitter(Qt::Vertical, this);
    m_fileSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);

    auto *localTabs = new QTabWidget(m_fileSplitter);
    localTabs->setObjectName("localTabs");
    m_localPanel = new FilePanel(FilePanel::Mode::Local, localTabs);
    m_localPanel->setLocalUploadRequestedHandler([this](const QString &localPath) {
        RemoteSession *session = currentRemoteSession();
        if (session != nullptr)
        {
            uploadLocalPath(*session, localPath);
        }
        else
        {
            showWarningMessage("上传失败", "请先连接远程会话。");
        }
    });
    m_localPanel->setRemoteFilesDroppedOnLocalHandler([this](const QStringList &remotePaths) {
        for (const QString &remotePath : remotePaths)
        {
            RemoteSession *session = currentRemoteSession();
            if (session != nullptr)
            {
                downloadRemotePath(*session, remotePath);
            }
        }
    });
    localTabs->addTab(m_localPanel, "本地：桌面");

    m_remoteTabs = new QTabWidget(m_fileSplitter);
    m_remoteTabs->setObjectName("remoteTabs");
    m_remoteTabs->setTabsClosable(false);
    m_remoteTabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_remoteTabs->tabBar(), &QTabBar::customContextMenuRequested, this, &MainWindow::showRemoteTabContextMenu);
    connect(m_remoteTabs, &QTabWidget::currentChanged, this, [this]() {
        RemoteSession *session = currentRemoteSession();
        m_remotePanel = session == nullptr ? dynamic_cast<FilePanel *>(m_remoteTabs->currentWidget()) : session->panel;
        updateRemoteConnectionActions();
        populateSessionManager();
    });
    connect(m_remoteTabs, &QTabWidget::tabCloseRequested, this, &MainWindow::closeRemoteTab);
    Q_UNUSED(dependencyCheck);
    m_remotePanel = nullptr;

    m_fileSplitter->addWidget(localTabs);
    m_fileSplitter->addWidget(m_remoteTabs);
    m_fileSplitter->setStretchFactor(0, 1);
    m_fileSplitter->setStretchFactor(1, 1);
    updateFileSplitterLayout();

    auto *bottomTabs = new QTabWidget(verticalSplitter);
    bottomTabs->setObjectName("bottomTabs");
    auto *transferTab = new QWidget(bottomTabs);
    auto *transferLayout = new QVBoxLayout(transferTab);
    transferLayout->setContentsMargins(0, 0, 0, 0);
    transferLayout->setSpacing(4);

    auto *transferToolbar = new QWidget(transferTab);
    auto *transferToolbarLayout = new QHBoxLayout(transferToolbar);
    transferToolbarLayout->setContentsMargins(0, 0, 0, 0);
    transferToolbarLayout->setSpacing(4);
    m_cancelTransferButton = new QPushButton("取消", transferToolbar);
    m_cancelTransferButton->setObjectName("transferCancelButton");
    m_cancelTransferButton->setIcon(fluentIcon("dismiss_circle"));
    m_retryTransferButton = new QPushButton("重试", transferToolbar);
    m_retryTransferButton->setObjectName("transferRetryButton");
    m_retryTransferButton->setIcon(fluentIcon("arrow_sync"));
    m_clearFinishedTransfersButton = new QPushButton("清理", transferToolbar);
    m_clearFinishedTransfersButton->setObjectName("transferClearFinishedButton");
    m_clearFinishedTransfersButton->setIcon(fluentIcon("delete"));
    transferToolbarLayout->addWidget(m_cancelTransferButton);
    transferToolbarLayout->addWidget(m_retryTransferButton);
    transferToolbarLayout->addWidget(m_clearFinishedTransfersButton);
    transferToolbarLayout->addStretch(1);

    m_transferTable = new QTreeWidget(transferTab);
    m_transferTable->setObjectName("transferTable");
    m_transferTable->setHeaderLabels({"名称", "状态", "进度", "大小", "本地路径", "<->", "远程路径", "速度", "估计剩余", "经过时间"});
    m_transferTable->header()->setStretchLastSection(true);
    m_transferTable->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_transferTable->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_transferTable->header()->setSectionResizeMode(2, QHeaderView::Fixed);
    m_transferTable->header()->resizeSection(2, 140);
    m_transferTable->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    transferLayout->addWidget(transferToolbar);
    transferLayout->addWidget(m_transferTable, 1);

    connect(m_transferTable, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateTransferActionButtons);
    connect(m_cancelTransferButton, &QPushButton::clicked, this, &MainWindow::cancelSelectedTransferJob);
    connect(m_retryTransferButton, &QPushButton::clicked, this, &MainWindow::retrySelectedTransferJob);
    connect(m_clearFinishedTransfersButton, &QPushButton::clicked, this, &MainWindow::clearFinishedTransferJobs);

    m_logView = new QTreeWidget(bottomTabs);
    m_logView->setObjectName("logView");
    m_logView->setHeaderLabels({"时间", "级别", "消息"});
    m_logView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_logView->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_logView->header()->setStretchLastSection(true);

    bottomTabs->addTab(transferTab, "传输");
    bottomTabs->addTab(m_logView, "日志");

    verticalSplitter->addWidget(m_fileSplitter);
    verticalSplitter->addWidget(bottomTabs);
    verticalSplitter->setStretchFactor(0, 5);
    verticalSplitter->setStretchFactor(1, 1);

    setCentralWidget(verticalSplitter);
    updateTransferActionButtons();
}

MainWindow::RemoteSession *MainWindow::createRemoteSession(const SiteProfile &profile, std::unique_ptr<RemoteFileSystem> fileSystem)
{
    if (m_remoteTabs == nullptr)
    {
        return nullptr;
    }

    auto session = std::make_unique<RemoteSession>();
    session->id = QString("%1-%2").arg(QString::fromStdString(profile.id)).arg(m_remoteSessions.size() + 1);
    session->profile = profile;
    session->fileSystem = std::move(fileSystem);
    session->connectionCanceled = std::make_shared<std::atomic_bool>(false);
    session->displayName = siteDisplayName(profile);
    session->panel = new FilePanel(FilePanel::Mode::RemotePlaceholder, m_remoteTabs);
    session->panel->setObjectName("remotePanel");

    RemoteSession *sessionPtr = session.get();
    sessionPtr->panel->setRemotePathRequestedHandler([this, sessionPtr](const QString &path, bool addToHistory) {
        loadRemotePath(*sessionPtr, path, addToHistory);
    });
    sessionPtr->panel->setRemoteRefreshRequestedHandler([this, sessionPtr]() {
        loadRemotePath(*sessionPtr,
            sessionPtr->currentPath.isEmpty() ? QString::fromStdString(sessionPtr->profile.defaultRemotePath) : sessionPtr->currentPath,
            false);
    });
    sessionPtr->panel->setRemoteCreateDirectoryRequestedHandler([this, sessionPtr](const QString &path) {
        createRemoteDirectory(*sessionPtr, path);
    });
    sessionPtr->panel->setRemoteCreateFileRequestedHandler([this, sessionPtr](const QString &path) {
        createRemoteFile(*sessionPtr, path);
    });
    sessionPtr->panel->setRemoteRemoveRequestedHandler([this, sessionPtr](const QString &path) {
        removeRemotePath(*sessionPtr, path);
    });
    sessionPtr->panel->setRemoteRenameRequestedHandler([this, sessionPtr](const QString &sourcePath, const QString &targetPath) {
        renameRemotePath(*sessionPtr, sourcePath, targetPath);
    });
    sessionPtr->panel->setRemoteDownloadRequestedHandler([this, sessionPtr](const QString &remotePath, bool isDirectory) {
        if (isDirectory)
        {
            downloadRemotePath(*sessionPtr, remotePath);
            return;
        }
        downloadRemoteFile(*sessionPtr, remotePath);
    });
    sessionPtr->panel->setLocalFilesDroppedOnRemoteHandler([this, sessionPtr](const QStringList &localPaths) {
        for (const QString &localPath : localPaths)
        {
            uploadLocalPath(*sessionPtr, localPath);
        }
    });
    sessionPtr->panel->setRemoteFilesDroppedOnRemoteHandler([this, sessionPtr](const QStringList &remotePaths, const QString &targetDirectory) {
        moveRemotePaths(*sessionPtr, remotePaths, targetDirectory);
    });

    const int tabIndex = m_remoteTabs->addTab(sessionPtr->panel, QString("远程：%1").arg(sessionPtr->displayName));
    m_remoteTabs->setTabText(tabIndex, QString("远程：%1").arg(sessionPtr->displayName));
    installRemoteTabCloseButton(tabIndex);
    m_remoteTabs->setCurrentIndex(tabIndex);
    m_remotePanel = sessionPtr->panel;
    m_remoteSessions.push_back(std::move(session));
    updateFileSplitterLayout();
    return sessionPtr;
}

MainWindow::RemoteSession *MainWindow::currentRemoteSession() const
{
    if (m_remoteTabs == nullptr)
    {
        return nullptr;
    }

    auto *panel = dynamic_cast<FilePanel *>(m_remoteTabs->currentWidget());
    return remoteSessionByPanel(panel);
}

MainWindow::RemoteSession *MainWindow::remoteSessionByPanel(FilePanel *panel) const
{
    if (panel == nullptr)
    {
        return nullptr;
    }

    for (const auto &session : m_remoteSessions)
    {
        if (session->panel == panel)
        {
            return session.get();
        }
    }
    return nullptr;
}

MainWindow::RemoteSession *MainWindow::remoteSessionById(const std::string &sessionId) const
{
    for (const auto &session : m_remoteSessions)
    {
        if (session->id.toStdString() == sessionId)
        {
            return session.get();
        }
    }
    return nullptr;
}

bool MainWindow::hasConnectingRemoteSession() const
{
    return std::any_of(m_remoteSessions.begin(), m_remoteSessions.end(), [](const std::unique_ptr<RemoteSession> &session) {
        return session != nullptr && session->connecting;
    });
}

QTreeWidget *MainWindow::createSessionManager()
{
    m_sessionTree = new QTreeWidget(this);
    m_sessionTree->setObjectName("sessionManagerTree");
    m_sessionTree->setHeaderHidden(true);
    m_sessionTree->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_sessionTree, &QTreeWidget::customContextMenuRequested, this, &MainWindow::showSessionManagerContextMenu);
    connect(m_sessionTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        fillQuickConnectFromItem(item);
        if (item == nullptr)
        {
            return;
        }

        const auto itemType = static_cast<SessionTreeItemType>(item->data(0, sessionItemTypeRole).toInt());
        if (itemType == SessionTreeItemType::Site)
        {
            connectSiteAtIndex(item->data(0, siteIndexRole).toInt());
        }
        else if (itemType == SessionTreeItemType::Recent)
        {
            connectRecentSession(
                item->data(0, siteIdRole).toString().toStdString(),
                item->data(0, remotePathRole).toString());
        }
    });
    return m_sessionTree;
}

void MainWindow::populateSessionManager()
{
    if (m_sessionTree == nullptr)
    {
        return;
    }

    m_sessionTree->clear();
    auto *sitesRoot = new QTreeWidgetItem(m_sessionTree, {"站点"});
    sitesRoot->setExpanded(true);

    std::map<QString, QTreeWidgetItem *> groupItems;
    const RemoteSession *currentSession = currentRemoteSession();
    const QString currentSiteId = currentSession == nullptr ? QString() : QString::fromStdString(currentSession->profile.id);
    for (int index = 0; index < static_cast<int>(m_sites.size()); ++index)
    {
        const SiteProfile &profile = m_sites.at(index);
        const QString groupName = QString::fromStdString(profile.group).trimmed().isEmpty()
            ? QString("未分组")
            : QString::fromStdString(profile.group).trimmed();
        QTreeWidgetItem *groupItem = nullptr;
        const auto group = groupItems.find(groupName);
        if (group == groupItems.end())
        {
            groupItem = new QTreeWidgetItem(sitesRoot, {groupName});
            groupItem->setData(0, sessionItemTypeRole, static_cast<int>(SessionTreeItemType::Group));
            groupItem->setData(0, groupNameRole, QString::fromStdString(profile.group));
            groupItem->setExpanded(true);
            groupItems[groupName] = groupItem;
        }
        else
        {
            groupItem = group->second;
        }

        QString label = siteDisplayName(profile);
        if (QString::fromStdString(profile.id) == currentSiteId)
        {
            label += currentSession->connecting ? "（当前，连接中）" : currentSession->connected ? "（当前）" : "（当前，断开）";
        }
        auto *item = new QTreeWidgetItem(groupItem, {label});
        item->setData(0, sessionItemTypeRole, static_cast<int>(SessionTreeItemType::Site));
        item->setData(0, siteIndexRole, index);
        item->setData(0, siteIdRole, QString::fromStdString(profile.id));
        item->setToolTip(0, QString("%1://%2:%3")
            .arg(QString::fromStdString(toString(profile.protocol)))
            .arg(QString::fromStdString(profile.host))
            .arg(profile.port));
    }

    auto *recentRoot = new QTreeWidgetItem(m_sessionTree, {"最近会话"});
    recentRoot->setExpanded(true);
    for (const RecentSession &recent : m_settings.recentSessions)
    {
        const int siteIndex = siteIndexById(recent.siteId);
        const QString displayName = QString::fromStdString(recent.displayName.empty() ? recent.siteId : recent.displayName);
        const QString lastPath = QString::fromStdString(recent.lastRemotePath.empty() ? "/" : recent.lastRemotePath);
        auto *item = new QTreeWidgetItem(recentRoot, {QString("%1  %2").arg(displayName, lastPath)});
        item->setData(0, sessionItemTypeRole, static_cast<int>(SessionTreeItemType::Recent));
        item->setData(0, siteIdRole, QString::fromStdString(recent.siteId));
        item->setData(0, remotePathRole, lastPath);
        item->setToolTip(0, QString("上次打开：%1\n路径：%2").arg(QString::fromStdString(recent.lastOpenedAt), lastPath));
        if (siteIndex < 0)
        {
            item->setDisabled(true);
            item->setText(0, item->text(0) + "（站点已删除）");
        }
        else if (QString::fromStdString(recent.siteId) == currentSiteId)
        {
            item->setText(0, item->text(0) + "（当前）");
        }
    }
}

void MainWindow::showSessionManagerContextMenu(const QPoint &position)
{
    if (m_sessionTree == nullptr)
    {
        return;
    }

    QTreeWidgetItem *item = m_sessionTree->itemAt(position);
    QMenu menu(m_sessionTree);
    menu.addAction(fluentIcon("folder_add"), "新建站点", this, [this]() {
        editSiteAtIndex(-1);
    });

    const auto itemType = item == nullptr
        ? SessionTreeItemType{}
        : static_cast<SessionTreeItemType>(item->data(0, sessionItemTypeRole).toInt());
    if (item != nullptr && itemType == SessionTreeItemType::Group)
    {
        const QString oldGroup = item->data(0, groupNameRole).toString();
        menu.addSeparator();
        menu.addAction(fluentIcon("edit"), "重命名分组", this, [this, oldGroup]() {
            promptRenameSiteGroup(oldGroup);
        });
    }
    else if (item != nullptr && itemType == SessionTreeItemType::Site)
    {
        const int index = item->data(0, siteIndexRole).toInt();
        menu.addSeparator();
        menu.addAction(fluentIcon("checkmark_circle"), "连接", this, [this, index]() {
            connectSiteAtIndex(index);
        });
        menu.addAction(fluentIcon("edit"), "编辑站点", this, [this, index]() {
            editSiteAtIndex(index);
        });
        menu.addAction(fluentIcon("delete"), "删除站点", this, [this, index]() {
            deleteSiteAtIndex(index);
        });
    }
    else if (item != nullptr && itemType == SessionTreeItemType::Recent && !item->isDisabled())
    {
        const std::string siteId = item->data(0, siteIdRole).toString().toStdString();
        const QString lastRemotePath = item->data(0, remotePathRole).toString();
        menu.addSeparator();
        menu.addAction(fluentIcon("checkmark_circle"), "连接", this, [this, siteId, lastRemotePath]() {
            connectRecentSession(siteId, lastRemotePath);
        });
    }

    menu.exec(m_sessionTree->viewport()->mapToGlobal(position));
}

void MainWindow::installRemoteTabCloseButton(int index)
{
    if (m_remoteTabs == nullptr || index < 0 || index >= m_remoteTabs->count())
    {
        return;
    }

    auto *closeButton = new QPushButton(QString::fromUtf8("×"), m_remoteTabs->tabBar());
    closeButton->setObjectName("remoteTabCloseButton");
    closeButton->setFlat(true);
    closeButton->setFixedSize(16, 16);
    closeButton->setCursor(Qt::ArrowCursor);
    closeButton->setToolTip("关闭会话");
    closeButton->setStyleSheet(
        "QPushButton#remoteTabCloseButton {"
        "border: none;"
        "border-radius: 8px;"
        "color: #666666;"
        "font-size: 13px;"
        "font-weight: 600;"
        "padding: 0;"
        "}"
        "QPushButton#remoteTabCloseButton:hover {"
        "background: #E5E7EB;"
        "color: #222222;"
        "}"
        "QPushButton#remoteTabCloseButton:pressed {"
        "background: #D1D5DB;"
        "}"
    );

    connect(closeButton, &QPushButton::clicked, this, [this, closeButton]() {
        if (m_remoteTabs == nullptr)
        {
            return;
        }
        const QPoint tabBarPosition = closeButton->mapTo(
            m_remoteTabs->tabBar(),
            closeButton->rect().center());
        const int tabIndex = m_remoteTabs->tabBar()->tabAt(tabBarPosition);
        if (tabIndex >= 0)
        {
            closeRemoteTab(tabIndex);
        }
    });

    m_remoteTabs->tabBar()->setTabButton(index, QTabBar::RightSide, closeButton);
}

void MainWindow::closeRemoteTab(int index)
{
    if (m_remoteTabs == nullptr || index < 0 || index >= m_remoteTabs->count())
    {
        return;
    }

    auto *panel = dynamic_cast<FilePanel *>(m_remoteTabs->widget(index));
    RemoteSession *session = remoteSessionByPanel(panel);
    if (session == nullptr)
    {
        if (m_remoteSessions.empty())
        {
            return;
        }
        QWidget *widget = m_remoteTabs->widget(index);
        m_remoteTabs->removeTab(index);
        if (widget != nullptr)
        {
            delete widget;
        }
    }
    else
    {
        if (session->connecting)
        {
            cancelRemoteConnection(*session);
        }
        if (hasRunningTransferForSession(session->id))
        {
            showWarningMessage("无法关闭会话", "当前远程会话仍有传输任务正在运行，请等待完成或先取消传输。");
            return;
        }
        if (session->fileSystem != nullptr && session->connected)
        {
            session->fileSystem->disconnect();
        }
        const QString displayName = session->displayName;
        QWidget *widget = session->panel;
        m_remoteTabs->removeTab(index);
        if (widget != nullptr)
        {
            delete widget;
        }
        m_remoteSessions.erase(std::remove_if(m_remoteSessions.begin(), m_remoteSessions.end(), [session](const std::unique_ptr<RemoteSession> &candidate) {
            return candidate.get() == session;
        }), m_remoteSessions.end());
        appendLog("INFO", QString("已关闭远程会话：%1").arg(displayName));
    }

    RemoteSession *current = currentRemoteSession();
    m_remotePanel = current == nullptr ? dynamic_cast<FilePanel *>(m_remoteTabs->currentWidget()) : current->panel;

    updateRemoteConnectionActions();
    updateFileSplitterLayout();
    populateSessionManager();
}

void MainWindow::showRemoteTabContextMenu(const QPoint &position)
{
    if (m_remoteTabs == nullptr)
    {
        return;
    }

    const int tabIndex = m_remoteTabs->tabBar()->tabAt(position);
    if (tabIndex < 0)
    {
        return;
    }

    auto *panel = dynamic_cast<FilePanel *>(m_remoteTabs->widget(tabIndex));
    RemoteSession *session = remoteSessionByPanel(panel);
    if (session == nullptr)
    {
        return;
    }

    QMenu menu(m_remoteTabs);
    QAction *reconnectAction = menu.addAction(fluentIcon("arrow_sync"), "重连");
    QAction *disconnectAction = menu.addAction(fluentIcon("dismiss_circle"), "断开");
    QAction *closeAction = menu.addAction(fluentIcon("dismiss_circle"), "关闭会话");
    reconnectAction->setEnabled(!session->connecting && !hasRunningTransferForSession(session->id));
    disconnectAction->setEnabled(session->connected && !session->connecting && !hasRunningTransferForSession(session->id));

    QAction *selected = menu.exec(m_remoteTabs->tabBar()->mapToGlobal(position));
    if (selected == reconnectAction)
    {
        m_remoteTabs->setCurrentIndex(tabIndex);
        reconnectRemoteSession(*session);
    }
    else if (selected == disconnectAction)
    {
        m_remoteTabs->setCurrentIndex(tabIndex);
        disconnectRemote();
    }
    else if (selected == closeAction)
    {
        closeRemoteTab(tabIndex);
    }
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

SiteProfile MainWindow::profileFromQuickConnect() const
{
    SiteProfile profile;
    profile.protocol = remoteProtocolFromString(m_protocolCombo->currentText().toStdString());
    profile.host = m_hostEdit->text().trimmed().toStdString();
    profile.port = static_cast<std::uint16_t>(m_portEdit->text().toUShort());
    if (profile.port == 0)
    {
        profile.port = defaultPortForProtocol(profile.protocol);
    }
    profile.username = m_userEdit->text().trimmed().toStdString();
    profile.password = m_passwordEdit->text().toStdString();
    QString remotePath = m_remotePathEdit->text().trimmed();
    if (remotePath.isEmpty())
    {
        remotePath = "/";
    }
    if (!remotePath.startsWith('/'))
    {
        remotePath.prepend('/');
    }
    profile.defaultRemotePath = remotePath.toStdString();
    profile.encoding = "UTF-8";
    profile.name = profile.host.empty()
        ? std::string("未命名站点")
        : protocolText(profile.protocol).toStdString() + " " + profile.host;
    profile.id = makeSiteId(profile);
    return profile;
}

void MainWindow::connectQuickProfile(bool saveProfile)
{
    try
    {
        SiteProfile profile = profileFromQuickConnect();
        if (profile.host.empty())
        {
            showWarningMessage("连接信息不完整", "请输入主机地址。");
            appendLog("WARN", "快速连接缺少主机地址");
            return;
        }

        if (saveProfile)
        {
            const auto sameSite = [&profile](const SiteProfile &site) {
                return site.protocol == profile.protocol
                    && site.host == profile.host
                    && site.port == profile.port
                    && site.username == profile.username;
            };
            auto existing = std::find_if(m_sites.begin(), m_sites.end(), sameSite);
            if (existing == m_sites.end())
            {
                m_sites.push_back(profile);
            }
            else
            {
                const std::string existingName = existing->name;
                const std::string existingGroup = existing->group;
                profile.id = existing->id;
                profile.name = existingName;
                profile.group = existingGroup;
                *existing = profile;
            }
            saveSites();
            appendLog("INFO", QString("已保存站点：%1").arg(siteDisplayName(profile)));
        }

        showRemoteProfile(profile);
    }
    catch (const std::exception &error)
    {
        appendLog("ERROR", QString("快速连接失败：%1").arg(error.what()));
        showCriticalMessage("快速连接失败", error.what());
    }
}

void MainWindow::showRemoteProfile(const SiteProfile &profile, const QString &initialRemotePath)
{
    if (hasConnectingRemoteSession())
    {
        const QString message = "已有远程连接正在进行，请等待完成或先取消当前连接。";
        appendLog("WARN", message);
        statusBar()->showMessage(message);
        showWarningMessage("连接进行中", message);
        return;
    }

    SiteProfile sessionProfile = profile;
    if (!initialRemotePath.trimmed().isEmpty())
    {
        sessionProfile.defaultRemotePath = normalizedRemotePath(initialRemotePath).toStdString();
    }

    appendLog("INFO", QString("连接远程站点：%1").arg(siteDisplayName(sessionProfile)));

    std::unique_ptr<RemoteFileSystem> fileSystem = m_testingRemoteFileSystem != nullptr
        ? std::move(m_testingRemoteFileSystem)
        : std::make_unique<CurlRemoteFileSystem>();

    RemoteSession *session = createRemoteSession(sessionProfile, std::move(fileSystem));
    if (session == nullptr)
    {
        showWarningMessage("连接失败", "无法创建远程会话。");
        return;
    }

    startRemoteConnection(*session);
}

void MainWindow::startRemoteConnection(RemoteSession &session)
{
    if (session.fileSystem == nullptr)
    {
        const QString message = "远程后端不可用，无法连接。";
        appendLog("ERROR", message);
        setRemoteConnectionState(session, false, message);
        showWarningMessage("连接失败", message);
        return;
    }

    session.connected = false;
    session.connecting = true;
    if (session.connectionCanceled == nullptr)
    {
        session.connectionCanceled = std::make_shared<std::atomic_bool>(false);
    }
    session.connectionCanceled->store(false);
    const QString sessionId = session.id;
    const SiteProfile profile = session.profile;
    const QString defaultRemotePath = QString::fromStdString(profile.defaultRemotePath.empty() ? "/" : profile.defaultRemotePath);
    std::unique_ptr<RemoteFileSystem> fileSystem = std::move(session.fileSystem);

    session.panel->setRemoteConnecting(QString("正在连接：%1").arg(siteDisplayName(profile)));
    if (m_remoteTabs != nullptr && session.panel != nullptr)
    {
        m_remoteTabs->setTabText(m_remoteTabs->indexOf(session.panel), QString("远程：%1（连接中）").arg(session.displayName));
    }
    appendLog("INFO", QString("开始连接远程站点：%1").arg(siteDisplayName(profile)));
    statusBar()->showMessage(QString("正在连接远程站点：%1").arg(siteDisplayName(profile)));
    updateRemoteConnectionActions();
    populateSessionManager();

    const std::shared_ptr<std::atomic_bool> canceled = session.connectionCanceled;
    QPointer<MainWindow> window(this);
    std::thread([window, sessionId, profile, defaultRemotePath, canceled, fileSystem = std::move(fileSystem)]() mutable {
        auto result = std::make_shared<RemoteConnectionResult>();
        result->profile = profile;
        result->requestedPath = defaultRemotePath;
        result->fileSystem = std::move(fileSystem);

        if (canceled->load())
        {
            result->canceled = true;
        }
        else
        {
            const RemoteOperationResult connectResult = result->fileSystem->connect(profile);
            if (!connectResult.success)
            {
                result->detail = QString::fromUtf8(connectResult.message.c_str());
                result->finalMessage = QString("连接站点“%1”失败。%2")
                    .arg(QString::fromStdString(profile.name.empty() ? profile.host : profile.name), userFacingRemoteError(result->detail));
            }
            else if (canceled->load())
            {
                result->fileSystem->disconnect();
                result->canceled = true;
            }
            else
            {
                result->connected = true;
                try
                {
                    result->items = result->fileSystem->listDirectory(defaultRemotePath.toStdString());
                    QStringList knownDirectories = ancestorRemoteDirectories(defaultRemotePath);
                    for (const QString &directory : ancestorRemoteDirectories(defaultRemotePath))
                    {
                        try
                        {
                            const std::vector<FileItem> siblingItems = result->fileSystem->listDirectory(directory.toStdString());
                            for (const FileItem &sibling : siblingItems)
                            {
                                if (sibling.type == FileItemType::Directory)
                                {
                                    knownDirectories << QString::fromStdString(sibling.path);
                                }
                            }
                        }
                        catch (const std::exception &)
                        {
                        }
                    }
                    for (const FileItem &item : result->items)
                    {
                        if (item.type == FileItemType::Directory)
                        {
                            knownDirectories << QString::fromStdString(item.path);
                        }
                    }
                    knownDirectories.removeDuplicates();
                    result->knownDirectories = knownDirectories;
                    result->loaded = true;
                }
                catch (const std::exception &error)
                {
                    result->detail = QString::fromUtf8(error.what());
                    result->finalMessage = QString("站点已连接，但无法加载默认目录“%1”。%2")
                        .arg(defaultRemotePath, userFacingRemoteError(result->detail));
                    result->fileSystem->disconnect();
                    result->connected = false;
                }
            }
        }

        if (canceled->load())
        {
            if (result->fileSystem != nullptr && result->fileSystem->isConnected())
            {
                result->fileSystem->disconnect();
            }
            result->canceled = true;
        }

        if (window != nullptr)
        {
            QMetaObject::invokeMethod(window, [window, sessionId, result]() {
                if (window != nullptr)
                {
                    window->finishRemoteConnection(sessionId, result);
                }
            }, Qt::QueuedConnection);
        }
    }).detach();
}

void MainWindow::finishRemoteConnection(const QString &sessionId, const std::shared_ptr<RemoteConnectionResult> &result)
{
    RemoteSession *session = remoteSessionById(sessionId.toStdString());
    if (session == nullptr || result == nullptr)
    {
        return;
    }

    session->connecting = false;
    if (result->canceled || (session->connectionCanceled != nullptr && session->connectionCanceled->load()))
    {
        setRemoteConnectionState(*session, false, "远程连接已取消。");
        appendLog("INFO", QString("远程连接已取消：%1").arg(session->displayName));
        updateRemoteConnectionActions();
        return;
    }

    if (!result->connected || !result->loaded)
    {
        const QString message = result->finalMessage.isEmpty()
            ? QString("连接站点“%1”失败。").arg(session->displayName)
            : result->finalMessage;
        appendLog("ERROR", result->detail.isEmpty() ? message : QString("%1 详细信息：%2").arg(message, result->detail));
        showWarningMessage(result->connected ? "默认目录加载失败" : "连接失败", message);
        setRemoteConnectionState(*session, false, message);
        updateRemoteConnectionActions();
        return;
    }

    session->fileSystem = std::move(result->fileSystem);
    session->connected = true;
    session->currentPath = result->requestedPath;
    session->panel->setRemoteKnownDirectories(result->knownDirectories);
    session->panel->setRemoteItems(
        session->currentPath,
        result->items,
        QString("%1 个项目").arg(result->items.size()),
        true);
    appendLog("INFO", QString("远程目录已加载：%1").arg(session->currentPath));
    setRemoteConnectionState(*session, true, QString("已连接：%1").arg(session->displayName));
    recordRecentSession(*session);
    updateRemoteConnectionActions();
}

void MainWindow::cancelRemoteConnection(RemoteSession &session)
{
    if (!session.connecting)
    {
        return;
    }

    if (session.connectionCanceled != nullptr)
    {
        session.connectionCanceled->store(true);
    }
    session.panel->setRemoteConnecting(QString("正在取消连接：%1").arg(session.displayName));
    statusBar()->showMessage(QString("正在取消远程连接：%1").arg(session.displayName));
    appendLog("INFO", QString("已请求取消远程连接：%1").arg(session.displayName));
    updateRemoteConnectionActions();
}

void MainWindow::reconnectRemoteSession(RemoteSession &session)
{
    if (session.connecting)
    {
        return;
    }
    if (hasRunningTransferForSession(session.id))
    {
        showWarningMessage("无法重连会话", "当前远程会话仍有传输任务正在运行，请等待完成或先取消传输。");
        return;
    }
    if (session.fileSystem != nullptr && session.connected)
    {
        session.fileSystem->disconnect();
    }

    session.fileSystem = m_testingRemoteFileSystem != nullptr
        ? std::move(m_testingRemoteFileSystem)
        : std::make_unique<CurlRemoteFileSystem>();
    session.currentPath.clear();
    session.connected = false;
    appendLog("INFO", QString("正在重连远程会话：%1").arg(session.displayName));
    startRemoteConnection(session);
}

bool MainWindow::loadRemotePath(RemoteSession &session, const QString &path, bool addToHistory, QString *errorMessage)
{
    if (session.connecting)
    {
        const QString message = "远程会话正在连接，暂时无法加载目录。";
        if (errorMessage != nullptr)
        {
            *errorMessage = message;
        }
        appendLog("WARN", message);
        return false;
    }

    if (!session.connected || session.fileSystem == nullptr)
    {
        const QString message = "远程会话未连接，无法加载目录。";
        if (errorMessage != nullptr)
        {
            *errorMessage = message;
        }
        appendLog("WARN", message);
        session.panel->setRemoteError(message);
        return false;
    }

    QString normalizedPath = path.trimmed();
    if (normalizedPath.isEmpty())
    {
        normalizedPath = "/";
    }
    if (!normalizedPath.startsWith('/'))
    {
        normalizedPath.prepend('/');
    }

    try
    {
        const std::vector<FileItem> items = session.fileSystem->listDirectory(normalizedPath.toStdString());
        QStringList knownDirectories = ancestorRemoteDirectories(normalizedPath);
        for (const QString &directory : ancestorRemoteDirectories(normalizedPath))
        {
            try
            {
                const std::vector<FileItem> siblingItems = session.fileSystem->listDirectory(directory.toStdString());
                for (const FileItem &sibling : siblingItems)
                {
                    if (sibling.type == FileItemType::Directory)
                    {
                        knownDirectories << QString::fromStdString(sibling.path);
                    }
                }
            }
            catch (const std::exception &)
            {
            }
        }
        for (const FileItem &item : items)
        {
            if (item.type == FileItemType::Directory)
            {
                knownDirectories << QString::fromStdString(item.path);
            }
        }
        knownDirectories.removeDuplicates();
        session.currentPath = normalizedPath;
        session.panel->setRemoteKnownDirectories(knownDirectories);
        session.panel->setRemoteItems(
            session.currentPath,
            items,
            QString("%1 个项目").arg(items.size()),
            addToHistory);
        appendLog("INFO", QString("远程目录已加载：%1").arg(session.currentPath));
        statusBar()->showMessage(QString("远程已连接：%1 %2")
            .arg(protocolText(session.profile.protocol), session.currentPath));
        recordRecentSession(session);
        return true;
    }
    catch (const std::exception &error)
    {
        const QString detail = QString::fromUtf8(error.what());
        const QString message = QString("无法加载远程目录“%1”。%2")
            .arg(normalizedPath, userFacingRemoteError(detail));
        if (errorMessage != nullptr)
        {
            *errorMessage = message;
        }
        appendLog("ERROR", QString("远程目录加载失败：%1").arg(detail));
        session.panel->setRemoteError(message);
        if (errorMessage == nullptr)
        {
            showWarningMessage("远程目录加载失败", message);
        }
        return false;
    }
}

bool MainWindow::loadRemotePath(const QString &path, bool addToHistory, QString *errorMessage)
{
    RemoteSession *session = currentRemoteSession();
    if (session == nullptr)
    {
        return false;
    }
    return loadRemotePath(*session, path, addToHistory, errorMessage);
}

void MainWindow::refreshRemote()
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr && session->connecting)
    {
        statusBar()->showMessage("远程会话正在连接，暂时不能刷新。");
        return;
    }
    if (session == nullptr || !session->connected)
    {
        if (m_localPanel != nullptr)
        {
            m_localPanel->refresh();
        }
        return;
    }

    loadRemotePath(*session,
        session->currentPath.isEmpty()
            ? QString::fromStdString(session->profile.defaultRemotePath)
            : session->currentPath,
        false);
}

void MainWindow::disconnectRemote()
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr && session->connecting)
    {
        cancelRemoteConnection(*session);
        return;
    }
    if (session == nullptr || !session->connected)
    {
        return;
    }
    if (hasRunningTransferForSession(session->id))
    {
        showWarningMessage("无法断开会话", "当前远程会话仍有传输任务正在运行，请等待完成或先取消传输。");
        return;
    }

    session->fileSystem->disconnect();
    setRemoteConnectionState(*session, false, "远程会话已断开。");
    appendLog("INFO", "远程会话已断开");
}

void MainWindow::createRemoteDirectory(RemoteSession &session, const QString &path)
{
    if (!session.connected)
    {
        session.panel->setRemoteError("远程会话未连接，无法新建目录。");
        return;
    }

    const RemoteOperationResult result = session.fileSystem->createDirectory(path.toStdString());
    if (!result.success)
    {
        const QString message = QString("远程新建目录失败：%1").arg(QString::fromStdString(result.message));
        appendLog("ERROR", message);
        session.panel->setRemoteError(message);
        showWarningMessage("远程新建目录失败", message);
        return;
    }

    appendLog("INFO", QString("远程目录已创建：%1").arg(path));
    loadRemotePath(session, session.currentPath, false);
}

void MainWindow::createRemoteFile(RemoteSession &session, const QString &path)
{
    if (!session.connected)
    {
        session.panel->setRemoteError("远程会话未连接，无法新建文件。");
        return;
    }

    const RemoteOperationResult result = session.fileSystem->createFile(path.toStdString());
    if (!result.success)
    {
        const QString message = QString("远程新建文件失败：%1").arg(QString::fromStdString(result.message));
        appendLog("ERROR", message);
        session.panel->setRemoteError(message);
        showWarningMessage("远程新建文件失败", message);
        return;
    }

    appendLog("INFO", QString("远程文件已创建：%1").arg(path));
    loadRemotePath(session, session.currentPath, false);
}

void MainWindow::removeRemotePath(RemoteSession &session, const QString &path)
{
    if (!session.connected)
    {
        session.panel->setRemoteError("远程会话未连接，无法删除。");
        return;
    }

    const QString deleteKey = session.id + "\n" + path;
    if (m_pendingRemoteDeletes.find(deleteKey) != m_pendingRemoteDeletes.end())
    {
        statusBar()->showMessage("该远程项目正在删除，请等待完成。");
        return;
    }

    m_pendingRemoteDeletes.insert(deleteKey);
    RemoteFileSystem *fileSystem = session.fileSystem.get();
    const QString sessionId = session.id;
    QPointer<MainWindow> window(this);
    QThread *thread = QThread::create([window, fileSystem, sessionId, path]() {
        QString errorMessage;
        std::function<bool(const QString &)> removeRecursive;
        removeRecursive = [&](const QString &currentPath) {
            if (currentPath.trimmed().isEmpty() || currentPath == "/")
            {
                errorMessage = "不允许删除远程根目录。";
                return false;
            }

            try
            {
                const std::vector<FileItem> children = fileSystem->listDirectory(currentPath.toStdString());
                for (const FileItem &child : children)
                {
                    if (!removeRecursive(QString::fromStdString(child.path)))
                    {
                        return false;
                    }
                }
            }
            catch (const std::exception &)
            {
            }

            const RemoteOperationResult result = fileSystem->remove(currentPath.toStdString());
            if (!result.success)
            {
                errorMessage = QString::fromStdString(result.message);
                return false;
            }
            return true;
        };

        removeRecursive(path);
        if (window != nullptr)
        {
            QMetaObject::invokeMethod(window.data(), [window, sessionId, path, errorMessage]() {
                if (window != nullptr)
                {
                    window->finishRemoteRemove(sessionId, path, errorMessage);
                }
            }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::removeRemotePath(const QString &path)
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr)
    {
        removeRemotePath(*session, path);
    }
}

void MainWindow::renameRemotePath(RemoteSession &session, const QString &sourcePath, const QString &targetPath)
{
    if (!session.connected)
    {
        session.panel->setRemoteError("远程会话未连接，无法重命名。");
        return;
    }

    const RemoteOperationResult result = session.fileSystem->rename(sourcePath.toStdString(), targetPath.toStdString());
    if (!result.success)
    {
        const QString message = QString("远程重命名失败：%1").arg(QString::fromStdString(result.message));
        appendLog("ERROR", message);
        session.panel->setRemoteError(message);
        showWarningMessage("远程重命名失败", message);
        return;
    }

    appendLog("INFO", QString("远程项目已重命名：%1 -> %2").arg(sourcePath, targetPath));
    loadRemotePath(session, session.currentPath, false);
}

bool MainWindow::removeRemotePathRecursive(RemoteSession &session, const QString &path, QString *errorMessage)
{
    if (path.trimmed().isEmpty() || path == "/")
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "不允许删除远程根目录。";
        }
        return false;
    }

    try
    {
        const std::vector<FileItem> children = session.fileSystem->listDirectory(path.toStdString());
        for (const FileItem &child : children)
        {
            if (!removeRemotePathRecursive(session, QString::fromStdString(child.path), errorMessage))
            {
                return false;
            }
        }
    }
    catch (const std::exception &)
    {
        // Listing fails for regular files on the current backends; delete it below.
    }

    const RemoteOperationResult result = session.fileSystem->remove(path.toStdString());
    if (!result.success)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString::fromStdString(result.message);
        }
        return false;
    }

    return true;
}

void MainWindow::moveRemotePaths(RemoteSession &session, const QStringList &sourcePaths, const QString &targetDirectory)
{
    if (!session.connected)
    {
        session.panel->setRemoteError("远程会话未连接，无法移动。");
        return;
    }

    QStringList movedPaths;
    for (const QString &sourcePath : sourcePaths)
    {
        if (sourcePath.isEmpty() || targetDirectory.isEmpty())
        {
            continue;
        }

        if (isSameOrDescendantRemotePath(sourcePath, targetDirectory))
        {
            const QString message = QString("不能把远程项目移动到自身或子目录：%1").arg(sourcePath);
            appendLog("WARN", message);
            showWarningMessage("远程移动失败", message);
            continue;
        }

        const QString targetPath = joinRemotePath(targetDirectory, remoteBaseName(sourcePath));
        if (sourcePath == targetPath)
        {
            continue;
        }

        const RemoteOperationResult result = session.fileSystem->rename(sourcePath.toStdString(), targetPath.toStdString());
        if (!result.success)
        {
            const QString message = QString("远程移动失败：%1").arg(QString::fromStdString(result.message));
            appendLog("ERROR", message);
            session.panel->setRemoteError(message);
            showWarningMessage("远程移动失败", message);
            continue;
        }
        movedPaths.append(QString("%1 -> %2").arg(sourcePath, targetPath));
    }

    if (!movedPaths.isEmpty())
    {
        appendLog("INFO", QString("远程项目已移动：%1").arg(movedPaths.join("; ")));
        loadRemotePath(session, session.currentPath, false);
    }
}

void MainWindow::setRemoteConnectionState(RemoteSession &session, bool connected, const QString &message)
{
    session.connected = connected;
    session.connecting = false;
    if (!connected)
    {
        session.currentPath.clear();
        session.panel->setRemoteDisconnected(message);
        if (m_remoteTabs != nullptr && session.panel != nullptr)
        {
            m_remoteTabs->setTabText(m_remoteTabs->indexOf(session.panel), QString("远程：%1（断开）").arg(session.displayName));
        }
    }
    else if (m_remoteTabs != nullptr && session.panel != nullptr)
    {
        m_remoteTabs->setTabText(
            m_remoteTabs->indexOf(session.panel),
            QString("远程：%1").arg(session.displayName));
    }

    updateRemoteConnectionActions();

    statusBar()->showMessage(message);
    populateSessionManager();
}

void MainWindow::finishRemoteRemove(const QString &sessionId, const QString &path, const QString &errorMessage)
{
    m_pendingRemoteDeletes.erase(sessionId + "\n" + path);
    RemoteSession *session = remoteSessionById(sessionId.toStdString());
    if (session == nullptr)
    {
        return;
    }

    if (!errorMessage.trimmed().isEmpty())
    {
        const QString message = QString("远程删除失败：%1").arg(errorMessage);
        appendLog("ERROR", message);
        session->panel->setRemoteError(message);
        showWarningMessage("远程删除失败", message);
        return;
    }

    appendLog("INFO", QString("远程项目已删除：%1").arg(path));
    loadRemotePath(*session, session->currentPath, false);
}

void MainWindow::updateFileSplitterLayout()
{
    if (m_fileSplitter == nullptr || m_remoteTabs == nullptr)
    {
        return;
    }

    if (m_remoteTabs->count() == 0)
    {
        m_remoteTabs->hide();
        m_fileSplitter->setSizes({1, 0});
        return;
    }

    m_remoteTabs->show();
    const int width = std::max(2, m_fileSplitter->width());
    m_fileSplitter->setSizes({width / 2, width - (width / 2)});
}

void MainWindow::updateRemoteConnectionActions()
{
    const RemoteSession *currentSession = currentRemoteSession();
    const bool currentConnecting = currentSession != nullptr && currentSession->connecting;
    const bool currentConnected = currentSession != nullptr && currentSession->connected;

    if (m_disconnectAction != nullptr)
    {
        m_disconnectAction->setEnabled(currentConnecting || currentConnected);
        m_disconnectAction->setText(currentConnecting ? "取消连接" : "断开");
    }
    if (m_refreshAction != nullptr)
    {
        m_refreshAction->setEnabled(currentConnected && !currentConnecting);
        m_refreshAction->setText("刷新远程");
    }
    if (m_connectButton != nullptr)
    {
        m_connectButton->setText(currentConnecting ? "取消" : "连接");
        m_connectButton->setIcon(fluentIcon(currentConnecting ? "dismiss_circle" : "checkmark_circle"));
    }
    if (m_saveSiteButton != nullptr)
    {
        m_saveSiteButton->setEnabled(!hasConnectingRemoteSession());
    }
}

void MainWindow::enqueueTransferJob(const TransferJob &job)
{
    m_transferQueue.enqueue(job);
    refreshTransferTable();
}

QString MainWindow::enqueueDirectoryTransferParent(TransferDirection direction, const QString &name, const QString &localPath, const QString &remotePath, const RemoteSession &session)
{
    TransferJob parent;
    parent.id = makeTransferJobId(direction == TransferDirection::Upload ? "upload-dir" : "download-dir");
    parent.name = name.toStdString();
    parent.kind = TransferJobKind::Directory;
    parent.direction = direction;
    parent.status = TransferStatus::Preparing;
    parent.localPath = localPath.toStdString();
    parent.remotePath = remotePath.toStdString();
    parent.sessionId = session.id.toStdString();
    parent.sessionName = session.displayName.toStdString();
    parent.totalBytes = 0;
    parent.transferredBytes = 0;
    parent.startedAtMs = currentEpochMillis();
    enqueueTransferJob(parent);
    return QString::fromStdString(parent.id);
}

void MainWindow::updateDirectoryTransferParents()
{
    for (const TransferJob &snapshot : m_transferQueue.jobs())
    {
        if (snapshot.kind != TransferJobKind::Directory)
        {
            continue;
        }

        TransferJob *parent = m_transferQueue.find(snapshot.id);
        if (parent == nullptr)
        {
            continue;
        }

        int totalChildren = 0;
        int finishedChildren = 0;
        std::int64_t totalBytes = 0;
        std::int64_t transferredBytes = 0;
        std::int64_t startedAtMs = 0;
        std::int64_t finishedAtMs = 0;
        double currentBytesPerSecond = 0.0;
        bool hasFailed = false;
        bool hasCanceled = false;
        bool hasRunning = false;
        bool hasPending = false;
        QStringList messages;

        for (const TransferJob &child : m_transferQueue.jobs())
        {
            if (child.parentId != parent->id)
            {
                continue;
            }

            ++totalChildren;
            if (child.totalBytes > 0)
            {
                totalBytes += child.totalBytes;
                transferredBytes += std::max<std::int64_t>(0, std::min(child.transferredBytes, child.totalBytes));
            }
            if (child.status == TransferStatus::Completed || child.status == TransferStatus::Failed || child.status == TransferStatus::Canceled)
            {
                ++finishedChildren;
            }
            if (child.startedAtMs > 0 && (startedAtMs == 0 || child.startedAtMs < startedAtMs))
            {
                startedAtMs = child.startedAtMs;
            }
            if (child.finishedAtMs > finishedAtMs)
            {
                finishedAtMs = child.finishedAtMs;
            }
            if (child.status == TransferStatus::Running)
            {
                currentBytesPerSecond += child.currentBytesPerSecond;
            }
            hasFailed = hasFailed || child.status == TransferStatus::Failed;
            hasCanceled = hasCanceled || child.status == TransferStatus::Canceled || child.status == TransferStatus::Canceling;
            hasRunning = hasRunning || child.status == TransferStatus::Running;
            hasPending = hasPending || child.status == TransferStatus::Pending;
            const QString childMessage = transferMessageText(child);
            if (!childMessage.isEmpty() && child.status != TransferStatus::Completed)
            {
                messages << childMessage;
            }
        }

        parent->totalChildren = totalChildren;
        parent->finishedChildren = finishedChildren;
        parent->totalBytes = totalBytes > 0 ? totalBytes : totalChildren;
        parent->transferredBytes = totalBytes > 0 ? transferredBytes : finishedChildren;
        if (parent->status == TransferStatus::Canceled || parent->status == TransferStatus::Canceling)
        {
            parent->currentBytesPerSecond = 0.0;
            parent->finishedAtMs = parent->finishedAtMs > 0 ? parent->finishedAtMs : currentEpochMillis();
            continue;
        }
        if (startedAtMs > 0)
        {
            parent->startedAtMs = startedAtMs;
        }
        parent->currentBytesPerSecond = currentBytesPerSecond;
        if (totalChildren == 0 && parent->status == TransferStatus::Preparing)
        {
            parent->totalBytes = 0;
            parent->transferredBytes = 0;
        }
        else if (totalChildren == 0)
        {
            parent->status = TransferStatus::Completed;
        }
        else if (hasFailed)
        {
            parent->status = TransferStatus::Failed;
        }
        else if (hasCanceled)
        {
            parent->status = TransferStatus::Canceled;
        }
        else if (finishedChildren == totalChildren)
        {
            parent->status = TransferStatus::Completed;
        }
        else if (hasRunning || finishedChildren > 0)
        {
            parent->status = TransferStatus::Running;
        }
        else if (hasPending)
        {
            parent->status = TransferStatus::Pending;
        }
        if (parent->status == TransferStatus::Completed
            || parent->status == TransferStatus::Failed
            || parent->status == TransferStatus::Canceled)
        {
            parent->finishedAtMs = parent->finishedAtMs > 0 ? parent->finishedAtMs : std::max(finishedAtMs, currentEpochMillis());
            parent->currentBytesPerSecond = 0.0;
        }
        parent->errorMessage = messages.isEmpty()
            ? QString("%1 / %2 个文件").arg(finishedChildren).arg(totalChildren).toStdString()
            : messages.join("; ").toStdString();
    }
}

void MainWindow::processTransferQueue()
{
    if (m_transferWorkerRunning)
    {
        return;
    }

    TransferJob *job = m_transferQueue.nextPending();
    if (job == nullptr)
    {
        refreshTransferTable();
        return;
    }

    RemoteSession *session = remoteSessionById(job->sessionId);
    if (session == nullptr || !session->connected || session->fileSystem == nullptr)
    {
        job->status = TransferStatus::Failed;
        job->errorMessage = "remote session is not available";
        refreshTransferTable();
        processTransferQueue();
        return;
    }

    job->status = TransferStatus::Running;
    job->errorMessage.clear();
    job->startedAtMs = currentEpochMillis();
    job->finishedAtMs = 0;
    job->lastProgressAtMs = job->startedAtMs;
    job->lastProgressBytes = std::max<std::int64_t>(0, job->transferredBytes);
    job->currentBytesPerSecond = 0.0;
    const TransferJob jobSnapshot = *job;
    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_transferCancelFlags[jobSnapshot.id] = cancelFlag;
    m_runningTransferJobId = QString::fromStdString(jobSnapshot.id);
    m_transferWorkerRunning = true;
    refreshTransferTable();

    RemoteFileSystem *fileSystem = session->fileSystem.get();
    QPointer<MainWindow> window(this);
    QThread *thread = QThread::create([window, fileSystem, jobSnapshot, cancelFlag]() {
        auto lastProgressAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(250);
        const QString jobId = QString::fromStdString(jobSnapshot.id);
        auto progress = [window, jobId, cancelFlag, &lastProgressAt](std::int64_t transferredBytes, std::int64_t totalBytes) {
            if (cancelFlag->load())
            {
                return false;
            }

            const auto now = std::chrono::steady_clock::now();
            const bool terminalProgress = totalBytes > 0 && transferredBytes >= totalBytes;
            if (terminalProgress || now - lastProgressAt >= std::chrono::milliseconds(100))
            {
                lastProgressAt = now;
                if (window != nullptr)
                {
                    QMetaObject::invokeMethod(window.data(), [window, jobId, transferredBytes, totalBytes]() {
                        if (window != nullptr)
                        {
                            window->handleTransferProgress(jobId, transferredBytes, totalBytes);
                        }
                    }, Qt::QueuedConnection);
                }
            }
            return !cancelFlag->load();
        };

        RemoteOperationResult result;
        if (fileSystem == nullptr)
        {
            result = {false, "remote session is not available"};
        }
        else if (jobSnapshot.direction == TransferDirection::Upload)
        {
            result = fileSystem->uploadFile(jobSnapshot.localPath, jobSnapshot.remotePath, progress);
        }
        else
        {
            result = fileSystem->downloadFile(jobSnapshot.remotePath, jobSnapshot.localPath, progress);
        }

        const bool canceled = cancelFlag->load();
        if (window != nullptr)
        {
            QMetaObject::invokeMethod(window.data(), [window, jobId, result, canceled]() {
                if (window != nullptr)
                {
                    window->handleTransferFinished(jobId, result, canceled);
                }
            }, Qt::QueuedConnection);
        }
    });
    m_transferThread = thread;
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::handleTransferProgress(const QString &jobId, std::int64_t transferredBytes, std::int64_t totalBytes)
{
    TransferJob *job = m_transferQueue.find(jobId.toStdString());
    if (job == nullptr
        || job->status == TransferStatus::Completed
        || job->status == TransferStatus::Failed
        || job->status == TransferStatus::Canceled)
    {
        return;
    }

    if (totalBytes > 0)
    {
        job->totalBytes = totalBytes;
    }
    const std::int64_t now = currentEpochMillis();
    const std::int64_t previousAt = job->lastProgressAtMs;
    const std::int64_t previousBytes = job->lastProgressBytes;
    job->transferredBytes = std::max<std::int64_t>(0, transferredBytes);
    if (previousAt > 0 && now > previousAt && job->transferredBytes >= previousBytes)
    {
        const double seconds = static_cast<double>(now - previousAt) / 1000.0;
        if (seconds > 0.0)
        {
            job->currentBytesPerSecond = static_cast<double>(job->transferredBytes - previousBytes) / seconds;
        }
    }
    job->lastProgressAtMs = now;
    job->lastProgressBytes = job->transferredBytes;
    refreshTransferTable();
}

void MainWindow::handleTransferFinished(const QString &jobId, const RemoteOperationResult &result, bool canceled)
{
    TransferJob *job = m_transferQueue.find(jobId.toStdString());
    if (job != nullptr)
    {
        const bool wasCanceling = job->status == TransferStatus::Canceling;
        if (canceled || wasCanceling)
        {
            job->status = TransferStatus::Canceled;
            job->errorMessage = "transfer canceled";
        }
        else if (result.success)
        {
            job->status = TransferStatus::Completed;
            if (job->totalBytes >= 0)
            {
                job->transferredBytes = job->totalBytes;
            }
            job->errorMessage = result.message;
        }
        else
        {
            job->status = TransferStatus::Failed;
            job->errorMessage = result.message;
        }
        job->finishedAtMs = currentEpochMillis();
        job->currentBytesPerSecond = 0.0;

        if (job->status == TransferStatus::Completed)
        {
            if (job->direction == TransferDirection::Upload)
            {
                RemoteSession *session = remoteSessionById(job->sessionId);
                if (session != nullptr && session->connected)
                {
                    loadRemotePath(*session, session->currentPath, false);
                }
            }
            else if (m_localPanel != nullptr)
            {
                m_localPanel->refresh();
            }
        }
    }

    m_transferCancelFlags.erase(jobId.toStdString());
    m_transferWorkerRunning = false;
    m_runningTransferJobId.clear();
    m_transferThread = nullptr;
    refreshTransferTable();
    processTransferQueue();
}

bool MainWindow::hasRunningTransferForSession(const QString &sessionId) const
{
    const QString deletePrefix = sessionId + "\n";
    for (const QString &deleteKey : m_pendingRemoteDeletes)
    {
        if (deleteKey.startsWith(deletePrefix))
        {
            return true;
        }
    }

    for (const TransferJob &job : m_transferQueue.jobs())
    {
        if (QString::fromStdString(job.sessionId) == sessionId
            && (job.status == TransferStatus::Preparing
                || job.status == TransferStatus::Running
                || job.status == TransferStatus::Canceling))
        {
            return true;
        }
    }
    return false;
}

void MainWindow::refreshTransferTable()
{
    if (m_transferTable == nullptr)
    {
        return;
    }

    updateDirectoryTransferParents();
    std::map<std::string, bool> expandedStates;
    for (int index = 0; index < m_transferTable->topLevelItemCount(); ++index)
    {
        QTreeWidgetItem *item = m_transferTable->topLevelItem(index);
        if (item != nullptr)
        {
            expandedStates[item->data(0, Qt::UserRole).toString().toStdString()] = item->isExpanded();
        }
    }

    auto installProgressBar = [this](QTreeWidgetItem *item, const TransferJob &job) {
        auto *bar = new QProgressBar(m_transferTable);
        bar->setRange(0, 100);
        bar->setValue(progressPercent(job));
        bar->setTextVisible(true);
        bar->setFormat("%p%");
        bar->setMinimumWidth(120);
        bar->setMaximumHeight(18);
        m_transferTable->setItemWidget(item, 2, bar);
    };

    m_transferTable->clear();
    std::map<std::string, QTreeWidgetItem *> parentItems;
    for (const TransferJob &job : m_transferQueue.jobs())
    {
        if (!job.parentId.empty())
        {
            continue;
        }

        auto *item = new QTreeWidgetItem({
            QString::fromStdString(job.name),
            transferStatusText(job.status),
            QString("%1%").arg(progressPercent(job)),
            transferSizeProgressText(job),
            QString::fromStdString(job.localPath),
            job.direction == TransferDirection::Upload ? "->" : "<-",
            QString::fromStdString(job.remotePath),
            transferSpeedText(job.currentBytesPerSecond),
            transferRemainingText(job),
            transferElapsedText(job)
        });
        item->setData(0, Qt::UserRole, QString::fromStdString(job.id));
        m_transferTable->addTopLevelItem(item);
        installProgressBar(item, job);
        if (job.kind == TransferJobKind::Directory)
        {
            parentItems[job.id] = item;
            const auto expanded = expandedStates.find(job.id);
            item->setExpanded(expanded != expandedStates.end() && expanded->second);
        }
    }

    for (const TransferJob &job : m_transferQueue.jobs())
    {
        if (job.parentId.empty())
        {
            continue;
        }

        const auto parent = parentItems.find(job.parentId);
        if (parent == parentItems.end())
        {
            continue;
        }

        auto *item = new QTreeWidgetItem(parent->second, {
            QString::fromStdString(job.name),
            transferStatusText(job.status),
            QString("%1%").arg(progressPercent(job)),
            transferSizeProgressText(job),
            QString::fromStdString(job.localPath),
            job.direction == TransferDirection::Upload ? "->" : "<-",
            QString::fromStdString(job.remotePath),
            transferSpeedText(job.currentBytesPerSecond),
            transferRemainingText(job),
            transferElapsedText(job)
        });
        item->setData(0, Qt::UserRole, QString::fromStdString(job.id));
        installProgressBar(item, job);
    }
    updateTransferActionButtons();
}

void MainWindow::updateTransferActionButtons()
{
    const QString selectedId = selectedTransferJobId();
    const TransferJob *selectedJob = selectedId.isEmpty() ? nullptr : m_transferQueue.find(selectedId.toStdString());

    const bool canCancel = selectedJob != nullptr
        && (selectedJob->status == TransferStatus::Preparing
            || selectedJob->status == TransferStatus::Pending
            || selectedJob->status == TransferStatus::Running);
    const bool canRetry = selectedJob != nullptr
        && (selectedJob->status == TransferStatus::Failed || selectedJob->status == TransferStatus::Canceled);
    const bool hasFinishedJobs = std::any_of(m_transferQueue.jobs().begin(), m_transferQueue.jobs().end(), [](const TransferJob &job) {
        return job.status == TransferStatus::Completed
            || job.status == TransferStatus::Failed
            || job.status == TransferStatus::Canceled;
    });

    if (m_cancelTransferButton != nullptr)
    {
        m_cancelTransferButton->setEnabled(canCancel);
    }
    if (m_retryTransferButton != nullptr)
    {
        m_retryTransferButton->setEnabled(canRetry);
    }
    if (m_clearFinishedTransfersButton != nullptr)
    {
        m_clearFinishedTransfersButton->setEnabled(hasFinishedJobs);
    }
}

QString MainWindow::selectedTransferJobId() const
{
    if (m_transferTable == nullptr || m_transferTable->selectedItems().isEmpty())
    {
        return {};
    }

    QTreeWidgetItem *item = m_transferTable->currentItem();
    if (item == nullptr)
    {
        item = m_transferTable->selectedItems().first();
    }

    return item == nullptr ? QString() : item->data(0, Qt::UserRole).toString();
}

void MainWindow::cancelSelectedTransferJob()
{
    const QString id = selectedTransferJobId();
    if (id.isEmpty())
    {
        return;
    }

    const TransferJob *selectedJob = m_transferQueue.find(id.toStdString());
    if (selectedJob != nullptr && selectedJob->kind == TransferJobKind::Directory)
    {
        bool changed = m_transferQueue.cancel(selectedJob->id, "用户取消目录传输");
        for (const TransferJob &snapshot : m_transferQueue.jobs())
        {
            if (snapshot.parentId == selectedJob->id)
            {
                changed = m_transferQueue.cancel(snapshot.id, "用户取消目录传输") || changed;
                const auto cancelFlag = m_transferCancelFlags.find(snapshot.id);
                if (cancelFlag != m_transferCancelFlags.end() && cancelFlag->second != nullptr)
                {
                    cancelFlag->second->store(true);
                }
            }
        }
        if (changed)
        {
            appendLog("INFO", QString("目录传输任务已取消：%1").arg(id));
            refreshTransferTable();
        }
        return;
    }

    if (m_transferQueue.cancel(id.toStdString(), "用户取消传输"))
    {
        const auto cancelFlag = m_transferCancelFlags.find(id.toStdString());
        if (cancelFlag != m_transferCancelFlags.end() && cancelFlag->second != nullptr)
        {
            cancelFlag->second->store(true);
        }
        appendLog("INFO", QString("传输任务已取消：%1").arg(id));
        refreshTransferTable();
    }
}

void MainWindow::retrySelectedTransferJob()
{
    const QString id = selectedTransferJobId();
    if (id.isEmpty())
    {
        return;
    }

    const TransferJob *selectedJob = m_transferQueue.find(id.toStdString());
    if (selectedJob != nullptr && selectedJob->kind == TransferJobKind::Directory)
    {
        int retried = 0;
        QStringList retrySourceIds;
        for (const TransferJob &snapshot : m_transferQueue.jobs())
        {
            if (snapshot.parentId != selectedJob->id
                || (snapshot.status != TransferStatus::Failed && snapshot.status != TransferStatus::Canceled))
            {
                continue;
            }
            retrySourceIds << QString::fromStdString(snapshot.id);
        }
        for (const QString &sourceId : retrySourceIds)
        {
            TransferJob *child = m_transferQueue.find(sourceId.toStdString());
            if (child != nullptr)
            {
                child->status = TransferStatus::Pending;
                child->transferredBytes = 0;
                child->startedAtMs = 0;
                child->finishedAtMs = 0;
                child->lastProgressAtMs = 0;
                child->lastProgressBytes = 0;
                child->currentBytesPerSecond = 0.0;
                child->errorMessage.clear();
                ++retried;
            }
        }
        if (retried > 0)
        {
            appendLog("INFO", QString("已重试 %1 个目录传输子任务：%2").arg(retried).arg(id));
            refreshTransferTable();
            processTransferQueue();
        }
        return;
    }

    const QString retryId = QString::fromStdString(makeTransferJobId("retry"));
    if (m_transferQueue.retry(id.toStdString(), retryId.toStdString()) == nullptr)
    {
        return;
    }

    appendLog("INFO", QString("传输任务已重试：%1 -> %2").arg(id, retryId));
    refreshTransferTable();
    processTransferQueue();
}

void MainWindow::clearFinishedTransferJobs()
{
    const std::size_t removed = m_transferQueue.clearFinished();
    if (removed == 0)
    {
        return;
    }

    appendLog("INFO", QString("已清理 %1 个传输历史任务").arg(removed));
    refreshTransferTable();
}

bool MainWindow::remotePathExists(RemoteSession &session, const QString &remotePath, FileItem *item)
{
    try
    {
        session.fileSystem->listDirectory(remotePath.toStdString());
        if (item != nullptr)
        {
            item->name = remoteBaseName(remotePath).toStdString();
            item->path = remotePath.toStdString();
            item->type = FileItemType::Directory;
        }
        return true;
    }
    catch (const std::exception &)
    {
    }

    const QString parent = remoteBaseName(remotePath).isEmpty()
        ? QString("/")
        : remotePath.left(remotePath.lastIndexOf('/')).isEmpty() ? QString("/") : remotePath.left(remotePath.lastIndexOf('/'));
    const QString name = remoteBaseName(remotePath);
    try
    {
        const std::vector<FileItem> siblings = session.fileSystem->listDirectory(parent.toStdString());
        for (const FileItem &candidate : siblings)
        {
            if (QString::fromStdString(candidate.name) == name)
            {
                if (item != nullptr)
                {
                    *item = candidate;
                }
                return true;
            }
        }
    }
    catch (const std::exception &)
    {
    }

    return false;
}

MainWindow::UploadConflictAction MainWindow::chooseUploadConflictAction(const QFileInfo &localInfo, const QString &remotePath, const FileItem &remoteItem) const
{
    if (m_dialogsSuppressedForTesting)
    {
        return UploadConflictAction::ContinueUpload;
    }

    QMessageBox messageBox(const_cast<MainWindow *>(this));
    messageBox.setWindowTitle("要上传的文件存在");
    messageBox.setIcon(QMessageBox::Question);
    messageBox.setText(QString("远程目标已包含同名%1。\n\n名称：%2\n目标：%3")
        .arg(remoteItem.type == FileItemType::Directory ? "文件夹" : "文件",
            localInfo.fileName(),
            remotePath));
    QPushButton *overwriteButton = messageBox.addButton("覆盖", QMessageBox::AcceptRole);
    QPushButton *skipButton = messageBox.addButton("跳过", QMessageBox::RejectRole);
    QPushButton *continueButton = messageBox.addButton("继续上传", QMessageBox::ActionRole);
    QPushButton *renameButton = messageBox.addButton("重命名", QMessageBox::ActionRole);
    QPushButton *cancelButton = messageBox.addButton("取消", QMessageBox::DestructiveRole);
    messageBox.setDefaultButton(continueButton);
    messageBox.exec();

    QAbstractButton *clicked = messageBox.clickedButton();
    if (clicked == overwriteButton)
    {
        return UploadConflictAction::Overwrite;
    }
    if (clicked == skipButton)
    {
        return UploadConflictAction::Skip;
    }
    if (clicked == renameButton)
    {
        return UploadConflictAction::Rename;
    }
    if (clicked == cancelButton)
    {
        return UploadConflictAction::Cancel;
    }
    return UploadConflictAction::ContinueUpload;
}

QString MainWindow::renamedRemotePathForUpload(const QString &remotePath, const QFileInfo &localInfo) const
{
    bool ok = false;
    const QString newName = QInputDialog::getText(
        const_cast<MainWindow *>(this),
        "重命名上传项目",
        "新名称：",
        QLineEdit::Normal,
        localInfo.fileName(),
        &ok).trimmed();
    if (!ok || newName.isEmpty() || newName.contains('/') || newName.contains('\\') || newName == "." || newName == "..")
    {
        return {};
    }

    const int slashIndex = remotePath.lastIndexOf('/');
    return slashIndex <= 0 ? "/" + newName : remotePath.left(slashIndex + 1) + newName;
}

void MainWindow::uploadLocalFile(RemoteSession &session, const QString &localPath)
{
    if (!session.connected)
    {
        showWarningMessage("上传失败", "请先连接远程会话。");
        return;
    }

    const QFileInfo localInfo(localPath);
    if (!localInfo.isFile())
    {
        showWarningMessage("上传失败", "当前只支持上传单个文件。");
        return;
    }

    TransferJob job;
    job.id = makeTransferJobId("upload");
    job.name = localInfo.fileName().toStdString();
    job.direction = TransferDirection::Upload;
    job.status = TransferStatus::Preparing;
    job.localPath = localPath.toStdString();
    job.remotePath = joinRemotePath(session.currentPath, localInfo.fileName()).toStdString();
    job.sessionId = session.id.toStdString();
    job.sessionName = session.displayName.toStdString();
    job.totalBytes = localInfo.size();
    job.transferredBytes = 0;
    enqueueTransferJob(job);
    startSingleFileUploadPreparation(session, QString::fromStdString(job.id));
}

void MainWindow::uploadLocalFile(const QString &localPath)
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr)
    {
        uploadLocalFile(*session, localPath);
    }
    else
    {
        showWarningMessage("上传失败", "请先连接远程会话。");
    }
}

void MainWindow::startSingleFileUploadPreparation(RemoteSession &session, const QString &jobId)
{
    TransferJob *job = m_transferQueue.find(jobId.toStdString());
    if (job == nullptr || !session.connected || session.fileSystem == nullptr)
    {
        return;
    }

    RemoteFileSystem *fileSystem = session.fileSystem.get();
    const QString remotePath = QString::fromStdString(job->remotePath);
    QPointer<MainWindow> window(this);
    QThread *thread = QThread::create([window, fileSystem, jobId, remotePath]() {
        bool exists = false;
        FileItem existingItem;
        QString errorMessage;
        QString parent = remoteBaseName(remotePath).isEmpty()
            ? QString("/")
            : remotePath.left(remotePath.lastIndexOf('/')).isEmpty() ? QString("/") : remotePath.left(remotePath.lastIndexOf('/'));
        const QString name = remoteBaseName(remotePath);
        try
        {
            const std::vector<FileItem> siblings = fileSystem->listDirectory(parent.toStdString());
            for (const FileItem &candidate : siblings)
            {
                if (QString::fromStdString(candidate.name) == name)
                {
                    exists = true;
                    existingItem = candidate;
                    break;
                }
            }
        }
        catch (const std::exception &error)
        {
            errorMessage = QString::fromUtf8(error.what());
        }

        if (window != nullptr)
        {
            QMetaObject::invokeMethod(window.data(), [window, jobId, exists, existingItem, errorMessage]() {
                if (window != nullptr)
                {
                    window->finishSingleFileUploadPreparation(jobId, exists, existingItem, errorMessage);
                }
            }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::finishSingleFileUploadPreparation(const QString &jobId, bool exists, const FileItem &existingItem, const QString &errorMessage)
{
    TransferJob *job = m_transferQueue.find(jobId.toStdString());
    if (job == nullptr || job->status == TransferStatus::Canceled || job->status == TransferStatus::Canceling)
    {
        refreshTransferTable();
        return;
    }
    if (!errorMessage.trimmed().isEmpty())
    {
        job->status = TransferStatus::Failed;
        job->errorMessage = errorMessage.toStdString();
        refreshTransferTable();
        showWarningMessage("上传失败", QString("检查远程目标失败：%1").arg(errorMessage));
        return;
    }

    auto queuePreparedJob = [this, job]() {
        job->status = TransferStatus::Pending;
        job->errorMessage.clear();
        refreshTransferTable();
        processTransferQueue();
    };

    if (!exists)
    {
        queuePreparedJob();
        return;
    }

    const QFileInfo localInfo(QString::fromStdString(job->localPath));
    const QString remotePath = QString::fromStdString(job->remotePath);
    const UploadConflictAction action = chooseUploadConflictAction(localInfo, remotePath, existingItem);
    if (action == UploadConflictAction::Cancel || action == UploadConflictAction::Skip || action == UploadConflictAction::ContinueUpload)
    {
        m_transferQueue.cancel(job->id, "用户取消上传");
        refreshTransferTable();
        return;
    }
    if (action == UploadConflictAction::Rename)
    {
        const QString renamedPath = renamedRemotePathForUpload(remotePath, localInfo);
        if (renamedPath.isEmpty())
        {
            m_transferQueue.cancel(job->id, "用户取消上传");
            refreshTransferTable();
            return;
        }
        job->remotePath = renamedPath.toStdString();
        queuePreparedJob();
        return;
    }

    RemoteSession *session = remoteSessionById(job->sessionId);
    if (session == nullptr || !session->connected || session->fileSystem == nullptr)
    {
        job->status = TransferStatus::Failed;
        job->errorMessage = "remote session is not available";
        refreshTransferTable();
        return;
    }

    RemoteFileSystem *fileSystem = session->fileSystem.get();
    const std::string jobIdString = job->id;
    QPointer<MainWindow> window(this);
    QThread *thread = QThread::create([window, fileSystem, jobIdString, remotePath]() {
        QString removeError;
        std::function<bool(const QString &)> removeRecursive;
        removeRecursive = [&](const QString &currentPath) {
            try
            {
                const std::vector<FileItem> children = fileSystem->listDirectory(currentPath.toStdString());
                for (const FileItem &child : children)
                {
                    if (!removeRecursive(QString::fromStdString(child.path)))
                    {
                        return false;
                    }
                }
            }
            catch (const std::exception &)
            {
            }
            const RemoteOperationResult result = fileSystem->remove(currentPath.toStdString());
            if (!result.success)
            {
                removeError = QString::fromStdString(result.message);
                return false;
            }
            return true;
        };
        removeRecursive(remotePath);
        if (window != nullptr)
        {
            QMetaObject::invokeMethod(window.data(), [window, jobIdString, removeError]() {
                if (window == nullptr)
                {
                    return;
                }
                TransferJob *preparedJob = window->m_transferQueue.find(jobIdString);
                if (preparedJob == nullptr)
                {
                    return;
                }
                if (!removeError.trimmed().isEmpty())
                {
                    preparedJob->status = TransferStatus::Failed;
                    preparedJob->errorMessage = removeError.toStdString();
                    window->refreshTransferTable();
                    window->showWarningMessage("上传失败", QString("覆盖远程项目失败：%1").arg(removeError));
                    return;
                }
                preparedJob->status = TransferStatus::Pending;
                preparedJob->errorMessage.clear();
                window->refreshTransferTable();
                window->processTransferQueue();
            }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::uploadLocalPath(RemoteSession &session, const QString &localPath)
{
    const QFileInfo localInfo(localPath);
    if (localInfo.isFile())
    {
        uploadLocalFile(session, localPath);
        return;
    }

    if (!localInfo.isDir())
    {
        showWarningMessage("上传失败", QString("本地项目不可用：%1").arg(localPath));
        return;
    }

    QString remoteDirectoryPath = joinRemotePath(session.currentPath, localInfo.fileName());
    FileItem existingItem;
    if (remotePathExists(session, remoteDirectoryPath, &existingItem))
    {
        const UploadConflictAction action = chooseUploadConflictAction(localInfo, remoteDirectoryPath, existingItem);
        if (action == UploadConflictAction::Cancel || action == UploadConflictAction::Skip)
        {
            return;
        }
        if (action == UploadConflictAction::Rename)
        {
            remoteDirectoryPath = renamedRemotePathForUpload(remoteDirectoryPath, localInfo);
            if (remoteDirectoryPath.isEmpty())
            {
                return;
            }
        }
        else if (action == UploadConflictAction::Overwrite)
        {
            QString errorMessage;
            if (!removeRemotePathRecursive(session, remoteDirectoryPath, &errorMessage))
            {
                showWarningMessage("上传文件夹失败", QString("覆盖远程项目失败：%1").arg(errorMessage));
                return;
            }
        }
    }
    const QString parentJobId = enqueueDirectoryTransferParent(
        TransferDirection::Upload,
        localInfo.fileName(),
        localPath,
        remoteDirectoryPath,
        session);
    startLocalDirectoryUploadPreparation(session, localPath, remoteDirectoryPath, parentJobId);
}

void MainWindow::startLocalDirectoryUploadPreparation(RemoteSession &session, const QString &localDirectoryPath, const QString &remoteDirectoryPath, const QString &parentJobId)
{
    if (!session.connected || session.fileSystem == nullptr)
    {
        handlePreparedDirectoryTransfer(parentJobId, {}, "请先连接远程会话。");
        return;
    }

    RemoteFileSystem *fileSystem = session.fileSystem.get();
    const QString sessionId = session.id;
    const QString sessionName = session.displayName;
    QPointer<MainWindow> window(this);

    QThread *thread = QThread::create([window, fileSystem, sessionId, sessionName, localDirectoryPath, remoteDirectoryPath, parentJobId]() {
        std::vector<TransferJob> jobs;
        QString errorMessage;

        auto ensureDirectory = [fileSystem](const QString &path, QString *error) {
            const RemoteOperationResult result = fileSystem->createDirectory(path.toStdString());
            if (result.success)
            {
                return true;
            }
            try
            {
                fileSystem->listDirectory(path.toStdString());
                return true;
            }
            catch (const std::exception &exception)
            {
                if (error != nullptr)
                {
                    *error = QString("远程目录创建失败：%1；确认目录失败：%2")
                        .arg(QString::fromStdString(result.message), QString::fromUtf8(exception.what()));
                }
                return false;
            }
        };

        const QFileInfo rootInfo(localDirectoryPath);
        if (!rootInfo.isDir())
        {
            errorMessage = QString("本地目录不存在：%1").arg(localDirectoryPath);
        }
        else if (ensureDirectory(remoteDirectoryPath, &errorMessage))
        {
            QDirIterator iterator(
                localDirectoryPath,
                QDir::AllEntries | QDir::NoDotAndDotDot,
                QDirIterator::Subdirectories);
            const QDir rootDir(localDirectoryPath);
            while (iterator.hasNext())
            {
                const QString localPath = iterator.next();
                const QFileInfo info(localPath);
                const QString relativePath = rootDir.relativeFilePath(localPath);
                const QString remotePath = joinRemotePath(remoteDirectoryPath, relativePath);
                if (info.isDir())
                {
                    if (!ensureDirectory(remotePath, &errorMessage))
                    {
                        break;
                    }
                    continue;
                }
                if (!info.isFile())
                {
                    continue;
                }

                TransferJob job;
                job.id = makeTransferJobId("upload");
                job.name = info.fileName().toStdString();
                job.kind = TransferJobKind::File;
                job.parentId = parentJobId.toStdString();
                job.direction = TransferDirection::Upload;
                job.status = TransferStatus::Pending;
                job.localPath = localPath.toStdString();
                job.remotePath = remotePath.toStdString();
                job.sessionId = sessionId.toStdString();
                job.sessionName = sessionName.toStdString();
                job.totalBytes = info.size();
                job.transferredBytes = 0;
                jobs.push_back(std::move(job));
            }
        }

        if (window != nullptr)
        {
            QMetaObject::invokeMethod(window.data(), [window, parentJobId, jobs = std::move(jobs), errorMessage]() {
                if (window != nullptr)
                {
                    window->handlePreparedDirectoryTransfer(parentJobId, jobs, errorMessage);
                }
            }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

bool MainWindow::enqueueLocalDirectoryUpload(RemoteSession &session, const QString &localDirectoryPath, const QString &remoteDirectoryPath, const QString &parentJobId, QString *errorMessage)
{
    if (!session.connected)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "请先连接远程会话。";
        }
        return false;
    }

    const QFileInfo rootInfo(localDirectoryPath);
    if (!rootInfo.isDir())
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("本地目录不存在：%1").arg(localDirectoryPath);
        }
        return false;
    }

    if (!ensureRemoteDirectory(session, remoteDirectoryPath, errorMessage))
    {
        return false;
    }

    QDirIterator iterator(
        localDirectoryPath,
        QDir::AllEntries | QDir::NoDotAndDotDot,
        QDirIterator::Subdirectories);
    const QDir rootDir(localDirectoryPath);
    while (iterator.hasNext())
    {
        const QString localPath = iterator.next();
        const QFileInfo info(localPath);
        const QString relativePath = rootDir.relativeFilePath(localPath);
        QString remotePath = joinRemotePath(remoteDirectoryPath, relativePath);
        if (info.isDir())
        {
            if (!ensureRemoteDirectory(session, remotePath, errorMessage))
            {
                return false;
            }
            continue;
        }

        if (!info.isFile())
        {
            continue;
        }

        FileItem existingItem;
        if (remotePathExists(session, remotePath, &existingItem))
        {
            const UploadConflictAction action = chooseUploadConflictAction(info, remotePath, existingItem);
            if (action == UploadConflictAction::Cancel)
            {
                if (errorMessage != nullptr)
                {
                    *errorMessage = "用户取消上传。";
                }
                return false;
            }
            if (action == UploadConflictAction::Skip || action == UploadConflictAction::ContinueUpload)
            {
                continue;
            }
            if (action == UploadConflictAction::Rename)
            {
                remotePath = renamedRemotePathForUpload(remotePath, info);
                if (remotePath.isEmpty())
                {
                    if (errorMessage != nullptr)
                    {
                        *errorMessage = "重命名上传项目已取消。";
                    }
                    return false;
                }
            }
            else if (action == UploadConflictAction::Overwrite)
            {
                QString removeError;
                if (!removeRemotePathRecursive(session, remotePath, &removeError))
                {
                    if (errorMessage != nullptr)
                    {
                        *errorMessage = QString("覆盖远程文件失败：%1").arg(removeError);
                    }
                    return false;
                }
            }
        }

        TransferJob job;
        job.id = makeTransferJobId("upload");
        job.name = info.fileName().toStdString();
        job.kind = TransferJobKind::File;
        job.parentId = parentJobId.toStdString();
        job.direction = TransferDirection::Upload;
        job.status = TransferStatus::Pending;
        job.localPath = localPath.toStdString();
        job.remotePath = remotePath.toStdString();
        job.sessionId = session.id.toStdString();
        job.sessionName = session.displayName.toStdString();
        job.totalBytes = info.size();
        job.transferredBytes = 0;
        enqueueTransferJob(job);
    }

    return true;
}

bool MainWindow::ensureRemoteDirectory(RemoteSession &session, const QString &remoteDirectoryPath, QString *errorMessage)
{
    const RemoteOperationResult result = session.fileSystem->createDirectory(remoteDirectoryPath.toStdString());
    if (result.success)
    {
        return true;
    }

    try
    {
        session.fileSystem->listDirectory(remoteDirectoryPath.toStdString());
        return true;
    }
    catch (const std::exception &error)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("远程目录创建失败：%1；确认目录失败：%2")
                .arg(QString::fromStdString(result.message), QString::fromUtf8(error.what()));
        }
        return false;
    }
}

void MainWindow::downloadRemoteFile(RemoteSession &session, const QString &remotePath)
{
    if (!session.connected)
    {
        showWarningMessage("下载失败", "请先连接远程会话。");
        return;
    }
    if (m_localPanel == nullptr || m_localPanel->currentPath().isEmpty())
    {
        showWarningMessage("下载失败", "本地目录不可用。");
        return;
    }

    const QFileInfo remoteInfo(remotePath);
    const QString localPath = QDir(m_localPanel->currentPath()).filePath(remoteInfo.fileName());

    TransferJob job;
    job.id = makeTransferJobId("download");
    job.name = remoteInfo.fileName().toStdString();
    job.direction = TransferDirection::Download;
    job.status = TransferStatus::Pending;
    job.localPath = localPath.toStdString();
    job.remotePath = remotePath.toStdString();
    job.sessionId = session.id.toStdString();
    job.sessionName = session.displayName.toStdString();
    enqueueTransferJob(job);
    processTransferQueue();
}

void MainWindow::downloadRemoteFile(const QString &remotePath)
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr)
    {
        downloadRemoteFile(*session, remotePath);
    }
    else
    {
        showWarningMessage("下载失败", "请先连接远程会话。");
    }
}

void MainWindow::downloadRemotePath(RemoteSession &session, const QString &remotePath)
{
    if (m_localPanel == nullptr || m_localPanel->currentPath().isEmpty())
    {
        showWarningMessage("下载失败", "本地目录不可用。");
        return;
    }

    const QString localPath = QDir(m_localPanel->currentPath()).filePath(remoteBaseName(remotePath));
    const QString parentJobId = enqueueDirectoryTransferParent(
        TransferDirection::Download,
        remoteBaseName(remotePath),
        localPath,
        remotePath,
        session);
    startRemoteDirectoryDownloadPreparation(session, remotePath, localPath, parentJobId);
}

void MainWindow::startRemoteDirectoryDownloadPreparation(RemoteSession &session, const QString &remoteDirectoryPath, const QString &localDirectoryPath, const QString &parentJobId)
{
    if (!session.connected || session.fileSystem == nullptr)
    {
        handlePreparedDirectoryTransfer(parentJobId, {}, "请先连接远程会话。");
        return;
    }

    RemoteFileSystem *fileSystem = session.fileSystem.get();
    const QString sessionId = session.id;
    const QString sessionName = session.displayName;
    QPointer<MainWindow> window(this);

    QThread *thread = QThread::create([window, fileSystem, sessionId, sessionName, remoteDirectoryPath, localDirectoryPath, parentJobId]() {
        std::vector<TransferJob> jobs;
        QString errorMessage;

        std::function<bool(const QString &, const QString &)> collectDirectory;
        collectDirectory = [&](const QString &remotePath, const QString &localPath) {
            std::vector<FileItem> children;
            try
            {
                children = fileSystem->listDirectory(remotePath.toStdString());
            }
            catch (const std::exception &exception)
            {
                errorMessage = QString::fromUtf8(exception.what());
                return false;
            }

            if (!QDir().mkpath(localPath))
            {
                errorMessage = QString("无法创建本地目录：%1").arg(localPath);
                return false;
            }

            for (const FileItem &child : children)
            {
                const QString childRemotePath = QString::fromStdString(child.path);
                const QString childLocalPath = QDir(localPath).filePath(QString::fromStdString(child.name));
                if (child.type == FileItemType::Directory)
                {
                    if (!collectDirectory(childRemotePath, childLocalPath))
                    {
                        return false;
                    }
                    continue;
                }
                if (child.type != FileItemType::File && child.type != FileItemType::Other)
                {
                    continue;
                }

                TransferJob job;
                job.id = makeTransferJobId("download");
                job.name = QString::fromStdString(child.name).toStdString();
                job.kind = TransferJobKind::File;
                job.parentId = parentJobId.toStdString();
                job.direction = TransferDirection::Download;
                job.status = TransferStatus::Pending;
                job.localPath = childLocalPath.toStdString();
                job.remotePath = childRemotePath.toStdString();
                job.sessionId = sessionId.toStdString();
                job.sessionName = sessionName.toStdString();
                job.totalBytes = child.size;
                job.transferredBytes = 0;
                jobs.push_back(std::move(job));
            }

            return true;
        };

        collectDirectory(remoteDirectoryPath, localDirectoryPath);

        if (window != nullptr)
        {
            QMetaObject::invokeMethod(window.data(), [window, parentJobId, jobs = std::move(jobs), errorMessage]() {
                if (window != nullptr)
                {
                    window->handlePreparedDirectoryTransfer(parentJobId, jobs, errorMessage);
                }
            }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void MainWindow::handlePreparedDirectoryTransfer(const QString &parentJobId, const std::vector<TransferJob> &jobs, const QString &errorMessage)
{
    TransferJob *parent = m_transferQueue.find(parentJobId.toStdString());
    if (parent == nullptr
        || parent->status == TransferStatus::Canceled
        || parent->status == TransferStatus::Canceling)
    {
        refreshTransferTable();
        return;
    }

    if (!errorMessage.trimmed().isEmpty())
    {
        parent->status = TransferStatus::Failed;
        parent->errorMessage = errorMessage.toStdString();
        refreshTransferTable();
        showWarningMessage(parent->direction == TransferDirection::Upload ? "上传文件夹失败" : "下载文件夹失败", errorMessage);
        return;
    }

    for (const TransferJob &job : jobs)
    {
        m_transferQueue.enqueue(job);
    }

    parent->status = jobs.empty() ? TransferStatus::Completed : TransferStatus::Pending;
    parent->errorMessage = jobs.empty() ? "0 / 0 个文件" : std::string();
    refreshTransferTable();
    processTransferQueue();
}

bool MainWindow::enqueueRemoteDirectoryDownload(RemoteSession &session, const QString &remoteDirectoryPath, const QString &localDirectoryPath, const QString &parentJobId, QString *errorMessage)
{
    if (!session.connected)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "请先连接远程会话。";
        }
        return false;
    }

    std::vector<FileItem> children;
    try
    {
        children = session.fileSystem->listDirectory(remoteDirectoryPath.toStdString());
    }
    catch (const std::exception &error)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString::fromUtf8(error.what());
        }
        return false;
    }

    if (!QDir().mkpath(localDirectoryPath))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("无法创建本地目录：%1").arg(localDirectoryPath);
        }
        return false;
    }

    for (const FileItem &child : children)
    {
        const QString childRemotePath = QString::fromStdString(child.path);
        const QString childLocalPath = QDir(localDirectoryPath).filePath(QString::fromStdString(child.name));
        if (child.type == FileItemType::Directory)
        {
            if (!enqueueRemoteDirectoryDownload(session, childRemotePath, childLocalPath, parentJobId, errorMessage))
            {
                return false;
            }
            continue;
        }

        if (child.type != FileItemType::File && child.type != FileItemType::Other)
        {
            continue;
        }

        TransferJob job;
        job.id = makeTransferJobId("download");
        job.name = QString::fromStdString(child.name).toStdString();
        job.kind = TransferJobKind::File;
        job.parentId = parentJobId.toStdString();
        job.direction = TransferDirection::Download;
        job.status = TransferStatus::Pending;
        job.localPath = childLocalPath.toStdString();
        job.remotePath = childRemotePath.toStdString();
        job.sessionId = session.id.toStdString();
        job.sessionName = session.displayName.toStdString();
        job.totalBytes = child.size;
        enqueueTransferJob(job);
    }

    return true;
}

int MainWindow::siteIndexById(const std::string &siteId) const
{
    for (int index = 0; index < static_cast<int>(m_sites.size()); ++index)
    {
        if (m_sites.at(index).id == siteId)
        {
            return index;
        }
    }
    return -1;
}

void MainWindow::connectSiteAtIndex(int index, const QString &initialRemotePath)
{
    if (index < 0 || index >= static_cast<int>(m_sites.size()))
    {
        const QString message = "站点不存在，无法连接。";
        appendLog("ERROR", message);
        statusBar()->showMessage(message);
        showWarningMessage("站点不存在", message);
        return;
    }

    try
    {
        showRemoteProfile(m_sites.at(index), initialRemotePath);
    }
    catch (const std::exception &error)
    {
        const QString message = QString("打开站点“%1”失败：%2")
            .arg(siteDisplayName(m_sites.at(index)), QString::fromUtf8(error.what()));
        appendLog("ERROR", message);
        showCriticalMessage("打开站点失败", message);
    }
}

void MainWindow::editSiteAtIndex(int index)
{
    SiteProfile profile;
    if (index >= 0 && index < static_cast<int>(m_sites.size()))
    {
        profile = m_sites.at(index);
    }
    else
    {
        profile.protocol = RemoteProtocol::Sftp;
        profile.port = defaultPortForProtocol(profile.protocol);
        profile.defaultRemotePath = "/";
        profile.encoding = "UTF-8";
    }

    if (!editSiteProfileDialog(this, profile))
    {
        return;
    }

    if (index >= 0 && index < static_cast<int>(m_sites.size()))
    {
        m_sites.at(index) = profile;
        appendLog("INFO", QString("已更新站点：%1").arg(siteDisplayName(profile)));
    }
    else
    {
        m_sites.push_back(profile);
        appendLog("INFO", QString("已新增站点：%1").arg(siteDisplayName(profile)));
    }
    saveSites();
}

void MainWindow::deleteSiteAtIndex(int index)
{
    if (index < 0 || index >= static_cast<int>(m_sites.size()))
    {
        return;
    }

    const SiteProfile profile = m_sites.at(index);
    if (!m_dialogsSuppressedForTesting)
    {
        const QMessageBox::StandardButton choice = QMessageBox::question(
            this,
            "删除站点",
            QString("确定删除站点“%1”吗？").arg(siteDisplayName(profile)));
        if (choice != QMessageBox::Yes)
        {
            return;
        }
    }

    m_sites.erase(m_sites.begin() + index);
    m_settings.recentSessions.erase(
        std::remove_if(m_settings.recentSessions.begin(), m_settings.recentSessions.end(), [&profile](const RecentSession &recent) {
            return recent.siteId == profile.id;
        }),
        m_settings.recentSessions.end());
    appendLog("INFO", QString("已删除站点：%1").arg(siteDisplayName(profile)));
    saveSites();
    saveSettings();
}

bool MainWindow::renameSiteGroup(const QString &oldGroup, const QString &newGroup)
{
    const QString normalizedOldGroup = oldGroup.trimmed();
    const QString normalizedNewGroup = newGroup.trimmed();
    if (normalizedOldGroup == normalizedNewGroup)
    {
        return false;
    }

    std::vector<std::string> changedSiteIds;
    for (SiteProfile &site : m_sites)
    {
        if (QString::fromStdString(site.group).trimmed() == normalizedOldGroup)
        {
            site.group = normalizedNewGroup.toStdString();
            changedSiteIds.push_back(site.id);
        }
    }

    if (changedSiteIds.empty())
    {
        return false;
    }

    for (const std::unique_ptr<RemoteSession> &session : m_remoteSessions)
    {
        if (session == nullptr)
        {
            continue;
        }
        if (std::find(changedSiteIds.begin(), changedSiteIds.end(), session->profile.id) != changedSiteIds.end())
        {
            session->profile.group = normalizedNewGroup.toStdString();
        }
    }

    saveSites();
    appendLog("INFO", QString("已重命名站点分组：%1 -> %2")
        .arg(normalizedOldGroup.isEmpty() ? "未分组" : normalizedOldGroup,
            normalizedNewGroup.isEmpty() ? "未分组" : normalizedNewGroup));
    return true;
}

void MainWindow::promptRenameSiteGroup(const QString &oldGroup)
{
    bool ok = false;
    const QString label = oldGroup.trimmed().isEmpty() ? QString("未分组") : oldGroup.trimmed();
    const QString newGroup = QInputDialog::getText(
        this,
        "重命名分组",
        QString("将分组“%1”重命名为：").arg(label),
        QLineEdit::Normal,
        oldGroup,
        &ok).trimmed();
    if (!ok)
    {
        return;
    }

    if (!renameSiteGroup(oldGroup, newGroup))
    {
        statusBar()->showMessage("分组名称未变化。");
    }
}

void MainWindow::recordRecentSession(const RemoteSession &session)
{
    if (session.profile.id.empty() || siteIndexById(session.profile.id) < 0)
    {
        return;
    }

    RecentSession recent;
    recent.siteId = session.profile.id;
    recent.lastRemotePath = session.currentPath.isEmpty() ? "/" : session.currentPath.toStdString();
    recent.displayName = session.displayName.toStdString();
    recent.lastOpenedAt = QDateTime::currentDateTime().toString(Qt::ISODate).toStdString();

    m_settings.recentSessions.erase(
        std::remove_if(m_settings.recentSessions.begin(), m_settings.recentSessions.end(), [&recent](const RecentSession &existing) {
            return existing.siteId == recent.siteId;
        }),
        m_settings.recentSessions.end());
    m_settings.recentSessions.insert(m_settings.recentSessions.begin(), recent);
    if (m_settings.recentSessions.size() > 10)
    {
        m_settings.recentSessions.resize(10);
    }
    saveSettings();
}

void MainWindow::connectRecentSession(const std::string &siteId, const QString &lastRemotePath)
{
    const int index = siteIndexById(siteId);
    if (index < 0)
    {
        const QString message = "最近会话引用的站点已删除，无法连接。";
        appendLog("WARN", message);
        statusBar()->showMessage(message);
        showWarningMessage("站点不存在", message);
        return;
    }

    connectSiteAtIndex(index, lastRemotePath);
}

void MainWindow::fillQuickConnectFromItem(QTreeWidgetItem *item)
{
    if (item == nullptr || static_cast<SessionTreeItemType>(item->data(0, sessionItemTypeRole).toInt()) != SessionTreeItemType::Site)
    {
        return;
    }

    const int index = item->data(0, siteIndexRole).toInt();
    if (index < 0 || index >= static_cast<int>(m_sites.size()))
    {
        return;
    }

    const SiteProfile &profile = m_sites.at(index);
    const int protocolIndex = findProtocolIndex(m_protocolCombo, profile.protocol);
    if (protocolIndex >= 0)
    {
        m_protocolCombo->setCurrentIndex(protocolIndex);
    }
    m_hostEdit->setText(QString::fromStdString(profile.host));
    m_portEdit->setText(QString::number(profile.port));
    m_userEdit->setText(QString::fromStdString(profile.username));
    m_passwordEdit->setText(QString::fromStdString(profile.password));
    m_remotePathEdit->setText(QString::fromStdString(profile.defaultRemotePath.empty() ? "/" : profile.defaultRemotePath));
    appendLog("INFO", QString("已填充站点：%1").arg(siteDisplayName(profile)));
}

QString MainWindow::siteDisplayName(const SiteProfile &profile) const
{
    const QString name = QString::fromStdString(profile.name);
    if (!name.trimmed().isEmpty())
    {
        return name;
    }

    return QString("%1 %2").arg(protocolText(profile.protocol), QString::fromStdString(profile.host));
}
