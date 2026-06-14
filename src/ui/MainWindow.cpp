#include "ui/MainWindow.h"

#include "logging/AppLogger.h"
#include "protocol/CurlRemoteFileSystem.h"
#include "core/TransferManager.h"
#include "ui/FilePanel.h"

#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <stdexcept>

#include <QAction>
#include <QComboBox>
#include <QDateTime>
#include <QDockWidget>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QSplitter>
#include <QStatusBar>
#include <QTabWidget>
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

std::string makeTransferJobId(const QString &prefix)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return prefix.toStdString() + "-" + std::to_string(millis);
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
    }

    return "等待中";
}

QString transferSizeText(std::int64_t bytes)
{
    if (bytes < 0)
    {
        return "";
    }

    return QString("%1 Bytes").arg(bytes);
}
}

MainWindow::MainWindow(const DependencyCheckResult &dependencyCheck, QWidget *parent)
    : QMainWindow(parent)
    , m_siteStore(dependencyCheck.siteConfigPath.empty() ? std::filesystem::path("config") / "sites.json" : dependencyCheck.siteConfigPath)
{
    setWindowTitle("DirBridge - Remote Folder Manager");
    resize(1380, 820);

    loadSites();
    setupCentralWorkspace(dependencyCheck);
    setupMenuBar();
    setupToolBar();
    setupQuickConnectBar();
    populateSessionManager();

    appendLog("INFO", "DirBridge UI started");
    appendLog("INFO", QString("site config: %1").arg(QString::fromStdString(m_siteStore.path().string())));
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

void MainWindow::downloadRemoteFileForTesting(const QString &remotePath)
{
    downloadRemoteFile(remotePath);
}

void MainWindow::removeRemotePathForTesting(const QString &path)
{
    removeRemotePath(path);
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
    m_siteStore.save(m_sites);
    populateSessionManager();
}

void MainWindow::setupMenuBar()
{
    QMenu *fileMenu = menuBar()->addMenu("文件(&F)");
    fileMenu->addAction("新建会话");
    fileMenu->addAction("保存快速连接", this, [this]() {
        connectQuickProfile(true);
    });
    fileMenu->addSeparator();
    fileMenu->addAction("退出", this, &QWidget::close);

    QMenu *editMenu = menuBar()->addMenu("编辑(&E)");
    editMenu->addAction("复制")->setEnabled(false);
    editMenu->addAction("粘贴")->setEnabled(false);

    QMenu *viewMenu = menuBar()->addMenu("查看(&V)");
    m_refreshAction = viewMenu->addAction("刷新", this, &MainWindow::refreshRemote);
    QAction *sessionManagerAction = nullptr;
    if (m_sessionDock != nullptr)
    {
        sessionManagerAction = m_sessionDock->toggleViewAction();
        sessionManagerAction->setText("会话管理器");
        sessionManagerAction->setShortcut(QKeySequence("Ctrl+1"));
        viewMenu->addAction(sessionManagerAction);
    }

    auto *fileTreeAction = viewMenu->addAction("文件树");
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

    QMenu *commandMenu = menuBar()->addMenu("命令(&C)");
    commandMenu->addAction("连接", this, [this]() {
        connectQuickProfile(false);
    });
    m_disconnectAction = commandMenu->addAction("断开", this, &MainWindow::disconnectRemote);
    m_disconnectAction->setEnabled(false);
    commandMenu->addAction(m_refreshAction);

    menuBar()->addMenu("工具(&T)")->addAction("选项");
    menuBar()->addMenu("窗口(&W)")->addAction("关闭当前标签");
    menuBar()->addMenu("帮助(&H)")->addAction("关于 DirBridge");
}

void MainWindow::setupToolBar()
{
    QToolBar *toolbar = addToolBar("主工具栏");
    toolbar->setMovable(false);
    toolbar->addAction("新建会话");
    toolbar->addAction("连接", this, [this]() {
        connectQuickProfile(false);
    });
    toolbar->addAction(m_disconnectAction);
    toolbar->addSeparator();
    toolbar->addAction(m_refreshAction);
    toolbar->addAction("设置");
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
    m_saveSiteButton = new QPushButton("保存站点", quickBar);
    m_saveSiteButton->setObjectName("quickSaveSiteButton");

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
        connectQuickProfile(false);
    });
    connect(m_saveSiteButton, &QPushButton::clicked, this, [this]() {
        connectQuickProfile(true);
    });
}

void MainWindow::setupCentralWorkspace(const DependencyCheckResult &dependencyCheck)
{
    m_sessionDock = new QDockWidget("Session Manager", this);
    m_sessionDock->setObjectName("SessionManagerDock");
    m_sessionDock->setWidget(createSessionManager());
    addDockWidget(Qt::LeftDockWidgetArea, m_sessionDock);

    auto *verticalSplitter = new QSplitter(Qt::Vertical, this);
    auto *fileSplitter = new QSplitter(Qt::Horizontal, verticalSplitter);

    auto *localTabs = new QTabWidget(fileSplitter);
    localTabs->setObjectName("localTabs");
    m_localPanel = new FilePanel(FilePanel::Mode::Local, localTabs);
    m_localPanel->setLocalUploadRequestedHandler([this](const QString &localPath) {
        uploadLocalFile(localPath);
    });
    m_localPanel->setRemoteFilesDroppedOnLocalHandler([this](const QStringList &remotePaths) {
        for (const QString &remotePath : remotePaths)
        {
            downloadRemoteFile(remotePath);
        }
    });
    localTabs->addTab(m_localPanel, "本地：桌面");

    m_remoteTabs = new QTabWidget(fileSplitter);
    m_remoteTabs->setObjectName("remoteTabs");
    connect(m_remoteTabs, &QTabWidget::currentChanged, this, [this]() {
        RemoteSession *session = currentRemoteSession();
        m_remotePanel = session == nullptr ? dynamic_cast<FilePanel *>(m_remoteTabs->currentWidget()) : session->panel;
        if (m_disconnectAction != nullptr)
        {
            m_disconnectAction->setEnabled(session != nullptr && session->connected);
        }
        if (m_refreshAction != nullptr)
        {
            m_refreshAction->setText(session != nullptr && session->connected ? "刷新远程" : "刷新本地");
        }
    });
    m_remotePanel = new FilePanel(FilePanel::Mode::RemotePlaceholder, m_remoteTabs);
    m_remotePanel->setObjectName("remotePanel");
    m_remotePanel->setRemoteSummary(
        QString::fromStdString(dependencyCheck.curl.version),
        dependencyCheck.curl.hasFtp,
        dependencyCheck.curl.hasSftp);
    m_remoteTabs->addTab(m_remotePanel, "远程：未连接");

    fileSplitter->addWidget(localTabs);
    fileSplitter->addWidget(m_remoteTabs);
    fileSplitter->setStretchFactor(0, 1);
    fileSplitter->setStretchFactor(1, 1);

    auto *bottomTabs = new QTabWidget(verticalSplitter);
    bottomTabs->setObjectName("bottomTabs");
    m_transferTable = new QTreeWidget(bottomTabs);
    m_transferTable->setObjectName("transferTable");
    m_transferTable->setHeaderLabels({"名称", "会话", "方向", "状态", "进度", "大小", "本地路径", "<->", "远程路径", "消息"});
    m_transferTable->header()->setStretchLastSection(true);
    m_transferTable->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_transferTable->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_transferTable->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_transferTable->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);

    m_logView = new QTreeWidget(bottomTabs);
    m_logView->setObjectName("logView");
    m_logView->setHeaderLabels({"时间", "级别", "消息"});
    m_logView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_logView->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_logView->header()->setStretchLastSection(true);

    bottomTabs->addTab(m_transferTable, "传输");
    bottomTabs->addTab(m_logView, "日志");

    verticalSplitter->addWidget(fileSplitter);
    verticalSplitter->addWidget(bottomTabs);
    verticalSplitter->setStretchFactor(0, 5);
    verticalSplitter->setStretchFactor(1, 1);

    setCentralWidget(verticalSplitter);

    TransferJob sampleJob;
    sampleJob.id = "sample-upload";
    sampleJob.name = "传输任务模型占位";
    sampleJob.direction = TransferDirection::Upload;
    sampleJob.status = TransferStatus::Canceled;
    sampleJob.localPath = "本地路径待选择";
    sampleJob.remotePath = "远程路径待选择";
    sampleJob.sessionId = "sample-session";
    sampleJob.sessionName = "会话待选择";
    enqueueTransferJob(sampleJob);
}

MainWindow::RemoteSession *MainWindow::createRemoteSession(const SiteProfile &profile, std::unique_ptr<RemoteFileSystem> fileSystem)
{
    if (m_remoteTabs == nullptr || fileSystem == nullptr)
    {
        return nullptr;
    }

    auto session = std::make_unique<RemoteSession>();
    session->id = QString("%1-%2").arg(QString::fromStdString(profile.id)).arg(m_remoteSessions.size() + 1);
    session->profile = profile;
    session->fileSystem = std::move(fileSystem);
    session->displayName = siteDisplayName(profile);
    const bool reusePlaceholderPanel = m_remoteSessions.empty() && m_remotePanel != nullptr && m_remoteTabs->indexOf(m_remotePanel) >= 0;
    session->panel = reusePlaceholderPanel ? m_remotePanel : new FilePanel(FilePanel::Mode::RemotePlaceholder, m_remoteTabs);
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
    sessionPtr->panel->setRemoteDownloadRequestedHandler([this, sessionPtr](const QString &remotePath) {
        downloadRemoteFile(*sessionPtr, remotePath);
    });
    sessionPtr->panel->setLocalFilesDroppedOnRemoteHandler([this, sessionPtr](const QStringList &localPaths) {
        for (const QString &localPath : localPaths)
        {
            uploadLocalFile(*sessionPtr, localPath);
        }
    });

    const int tabIndex = reusePlaceholderPanel
        ? m_remoteTabs->indexOf(sessionPtr->panel)
        : m_remoteTabs->addTab(sessionPtr->panel, QString("远程：%1").arg(sessionPtr->displayName));
    m_remoteTabs->setTabText(tabIndex, QString("远程：%1").arg(sessionPtr->displayName));
    m_remoteTabs->setCurrentIndex(tabIndex);
    m_remotePanel = sessionPtr->panel;
    m_remoteSessions.push_back(std::move(session));
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

QTreeWidget *MainWindow::createSessionManager()
{
    m_sessionTree = new QTreeWidget(this);
    m_sessionTree->setHeaderHidden(true);
    connect(m_sessionTree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        fillQuickConnectFromItem(item);
        if (item != nullptr && item->data(0, Qt::UserRole).isValid())
        {
            try
            {
                showRemoteProfile(m_sites.at(item->data(0, Qt::UserRole).toInt()));
            }
            catch (const std::exception &error)
            {
                appendLog("ERROR", QString("打开站点失败：%1").arg(error.what()));
                showCriticalMessage("打开站点失败", error.what());
            }
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
    auto *root = new QTreeWidgetItem(m_sessionTree, {"所有会话"});
    for (int index = 0; index < static_cast<int>(m_sites.size()); ++index)
    {
        const SiteProfile &profile = m_sites.at(index);
        auto *item = new QTreeWidgetItem(root, {siteDisplayName(profile)});
        item->setData(0, Qt::UserRole, index);
        item->setToolTip(0, QString("%1://%2:%3")
            .arg(QString::fromStdString(toString(profile.protocol)))
            .arg(QString::fromStdString(profile.host))
            .arg(profile.port));
    }
    root->setExpanded(true);
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
                profile.id = existing->id;
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

void MainWindow::showRemoteProfile(const SiteProfile &profile)
{
    appendLog("INFO", QString("连接远程站点：%1").arg(siteDisplayName(profile)));

    std::unique_ptr<RemoteFileSystem> fileSystem = m_testingRemoteFileSystem != nullptr
        ? std::move(m_testingRemoteFileSystem)
        : std::make_unique<CurlRemoteFileSystem>();

    RemoteSession *session = createRemoteSession(profile, std::move(fileSystem));
    if (session == nullptr)
    {
        showWarningMessage("连接失败", "无法创建远程会话。");
        return;
    }

    const RemoteOperationResult result = session->fileSystem->connect(profile);
    if (!result.success)
    {
        appendLog("ERROR", QString("连接失败：%1").arg(QString::fromStdString(result.message)));
        showWarningMessage("连接失败", QString::fromStdString(result.message));
        setRemoteConnectionState(*session, false, QString("连接失败：%1").arg(QString::fromStdString(result.message)));
        return;
    }

    session->connected = true;
    QString loadError;
    const QString defaultRemotePath = QString::fromStdString(profile.defaultRemotePath);
    if (!loadRemotePath(*session, defaultRemotePath, true, &loadError))
    {
        session->fileSystem->disconnect();
        const QString message = loadError.isEmpty()
            ? QString("连接失败：无法加载默认目录 %1").arg(defaultRemotePath)
            : QString("连接失败：无法加载默认目录 %1。%2").arg(defaultRemotePath, loadError);
        setRemoteConnectionState(*session, false, message);
        return;
    }

    setRemoteConnectionState(*session, true, QString("已连接：%1").arg(siteDisplayName(profile)));
}

bool MainWindow::loadRemotePath(RemoteSession &session, const QString &path, bool addToHistory, QString *errorMessage)
{
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
        session.currentPath = normalizedPath;
        session.panel->setRemoteItems(
            session.currentPath,
            items,
            QString("%1 已连接：%2，%3 个项目")
                .arg(protocolText(session.profile.protocol))
                .arg(session.currentPath)
                .arg(items.size()),
            addToHistory);
        appendLog("INFO", QString("远程目录已加载：%1").arg(session.currentPath));
        statusBar()->showMessage(QString("远程已连接：%1 %2")
            .arg(protocolText(session.profile.protocol), session.currentPath));
        return true;
    }
    catch (const std::exception &error)
    {
        const QString message = QString("远程目录加载失败：%1").arg(error.what());
        if (errorMessage != nullptr)
        {
            *errorMessage = message;
        }
        appendLog("ERROR", message);
        session.panel->setRemoteError(message);
        showWarningMessage("远程目录加载失败", message);
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
    if (session == nullptr || !session->connected)
    {
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

    const RemoteOperationResult result = session.fileSystem->remove(path.toStdString());
    if (!result.success)
    {
        const QString message = QString("远程删除失败：%1").arg(QString::fromStdString(result.message));
        appendLog("ERROR", message);
        session.panel->setRemoteError(message);
        showWarningMessage("远程删除失败", message);
        return;
    }

    appendLog("INFO", QString("远程项目已删除：%1").arg(path));
    loadRemotePath(session, session.currentPath, false);
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

void MainWindow::setRemoteConnectionState(RemoteSession &session, bool connected, const QString &message)
{
    session.connected = connected;
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

    if (m_disconnectAction != nullptr)
    {
        const RemoteSession *currentSession = currentRemoteSession();
        m_disconnectAction->setEnabled(currentSession != nullptr && currentSession->connected);
    }
    if (m_refreshAction != nullptr)
    {
        const RemoteSession *currentSession = currentRemoteSession();
        m_refreshAction->setText(currentSession != nullptr && currentSession->connected ? "刷新远程" : "刷新本地");
    }

    statusBar()->showMessage(message);
}

void MainWindow::enqueueTransferJob(const TransferJob &job)
{
    m_transferQueue.enqueue(job);
    refreshTransferTable();
}

void MainWindow::processTransferQueue()
{
    TransferManager manager([this](const TransferJob &job) -> RemoteFileSystem * {
        RemoteSession *session = remoteSessionById(job.sessionId);
        if (session == nullptr || !session->connected)
        {
            return nullptr;
        }
        return session->fileSystem.get();
    }, m_transferQueue);
    manager.setQueueChangedCallback([this]() {
        refreshTransferTable();
    });
    manager.processPending();
    refreshTransferTable();
}

void MainWindow::refreshTransferTable()
{
    if (m_transferTable == nullptr)
    {
        return;
    }

    m_transferTable->clear();
    for (const TransferJob &job : m_transferQueue.jobs())
    {
        auto *item = new QTreeWidgetItem(m_transferTable, {
            QString::fromStdString(job.name),
            QString::fromStdString(job.sessionName),
            transferDirectionText(job.direction),
            transferStatusText(job.status),
            QString("%1%").arg(progressPercent(job)),
            transferSizeText(job.totalBytes),
            QString::fromStdString(job.localPath),
            job.direction == TransferDirection::Upload ? "->" : "<-",
            QString::fromStdString(job.remotePath),
            QString::fromStdString(job.errorMessage)
        });
        item->setData(0, Qt::UserRole, QString::fromStdString(job.id));
        m_transferTable->addTopLevelItem(item);
    }
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
    job.status = TransferStatus::Pending;
    job.localPath = localPath.toStdString();
    job.remotePath = joinRemotePath(session.currentPath, localInfo.fileName()).toStdString();
    job.sessionId = session.id.toStdString();
    job.sessionName = session.displayName.toStdString();
    job.totalBytes = localInfo.size();
    job.transferredBytes = 0;
    enqueueTransferJob(job);
    processTransferQueue();

    const TransferJob *finishedJob = m_transferQueue.find(job.id);
    if (finishedJob == nullptr)
    {
        return;
    }
    if (finishedJob->status == TransferStatus::Completed)
    {
        appendLog("INFO", QString("上传完成：%1").arg(QString::fromStdString(finishedJob->remotePath)));
        loadRemotePath(session, session.currentPath, false);
        return;
    }

    appendLog("ERROR", QString("上传失败：%1").arg(QString::fromStdString(finishedJob->errorMessage)));
    showWarningMessage("上传失败", QString::fromStdString(finishedJob->errorMessage));
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

    const TransferJob *finishedJob = m_transferQueue.find(job.id);
    if (finishedJob == nullptr)
    {
        return;
    }
    if (finishedJob->status == TransferStatus::Completed)
    {
        const QFileInfo downloadedInfo(localPath);
        TransferJob completedJob = *finishedJob;
        completedJob.totalBytes = downloadedInfo.exists() ? downloadedInfo.size() : -1;
        completedJob.transferredBytes = completedJob.totalBytes;
        m_transferQueue.update(completedJob);
        refreshTransferTable();
        appendLog("INFO", QString("下载完成：%1").arg(localPath));
        m_localPanel->refresh();
        return;
    }

    appendLog("ERROR", QString("下载失败：%1").arg(QString::fromStdString(finishedJob->errorMessage)));
    showWarningMessage("下载失败", QString::fromStdString(finishedJob->errorMessage));
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

void MainWindow::fillQuickConnectFromItem(QTreeWidgetItem *item)
{
    if (item == nullptr || !item->data(0, Qt::UserRole).isValid())
    {
        return;
    }

    const int index = item->data(0, Qt::UserRole).toInt();
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
