#include "ui/MainWindow.h"

#include "ui/FilePanel.h"
#include "ui/panel_shared.h"
#include "ui/window_shared.h"

#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QDir>
#include <QFileInfo>
#include <QHeaderView>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStatusBar>
#include <QStyle>
#include <QTabBar>
#include <QTabWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>
#include <QWidget>

using namespace window_shared;

namespace
{
QString localTabTitle(const QString &path)
{
    const QString cleanedPath = QDir::cleanPath(path);
    const QString nativePath = QDir::toNativeSeparators(cleanedPath);
    const QFileInfo info(cleanedPath);
    if (cleanedPath.isEmpty() || QDir(cleanedPath).isRoot() || info.fileName().isEmpty())
    {
        return QString("本地：%1").arg(nativePath);
    }

    return QString("本地：%1").arg(info.fileName());
}
}

/**
 * @brief 构建主菜单，并绑定窗口级常用操作。
 */
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

    m_fileTreeAction = viewMenu->addAction(fluentIcon("folder_add"), "文件树");
    m_fileTreeAction->setCheckable(true);
    m_fileTreeAction->setChecked(m_localPanel == nullptr || m_localPanel->isFileTreeVisible());
    connect(m_fileTreeAction, &QAction::toggled, this, [this](bool visible) {
        setAllFileTreesVisible(visible);
    });

    m_disconnectAction = new QAction(fluentIcon("dismiss_circle"), "断开", this);
    connect(m_disconnectAction, &QAction::triggered, this, &MainWindow::disconnectRemote);
    m_disconnectAction->setEnabled(false);
    m_refreshAction = new QAction(fluentIcon("arrow_sync"), "刷新远程", this);
    connect(m_refreshAction, &QAction::triggered, this, &MainWindow::refreshRemote);

    menuBar()->addMenu("帮助(&H)")->addAction(fluentIcon("info"), "关于 DirBridge", this, &MainWindow::showAboutDialog);
}

/**
 * @brief 显示关于对话框，展示应用版本与公开项目信息。
 */
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

/**
 * @brief 预留工具栏初始化入口，便于后续集中扩展。
 */
void MainWindow::setupToolBar()
{
}

/**
 * @brief 构建快速连接栏，并绑定连接/保存站点相关交互。
 */
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

    auto addGroupGap = [quickBar]() {
        auto *spacer = new QWidget(quickBar);
        spacer->setFixedWidth(8);
        quickBar->addWidget(spacer);
    };
    auto addFieldLabel = [quickBar](const QString &text) {
        auto *label = new QLabel(text, quickBar);
        label->setContentsMargins(0, 0, 4, 0);
        quickBar->addWidget(label);
    };

    addGroupGap();
    addFieldLabel("协议");
    quickBar->addWidget(m_protocolCombo);
    addGroupGap();
    addFieldLabel("主机");
    quickBar->addWidget(m_hostEdit);
    addGroupGap();
    addFieldLabel("端口");
    quickBar->addWidget(m_portEdit);
    addGroupGap();
    addFieldLabel("用户");
    quickBar->addWidget(m_userEdit);
    addGroupGap();
    addFieldLabel("密码");
    quickBar->addWidget(m_passwordEdit);
    addGroupGap();
    addFieldLabel("路径");
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
        ensureSiteGroupStored(profile.group);
        saveSites();
        appendLog("INFO", QString("已新建站点：%1").arg(siteDisplayName(profile)));
    });
}

/**
 * @brief 组装主窗口中央区域，包括本地/远程面板、传输区和日志区。
 * @param dependencyCheck 启动依赖检查结果，当前仅用于保持构造入口一致。
 */
void MainWindow::setupCentralWorkspace(const DependencyCheckResult &dependencyCheck)
{
    m_sessionDock = new QDockWidget("Session Manager", this);
    m_sessionDock->setObjectName("SessionManagerDock");
    m_sessionDock->setWidget(createSessionManager());
    addDockWidget(Qt::LeftDockWidgetArea, m_sessionDock);

    m_workspaceSplitter = new QSplitter(Qt::Vertical, this);
    m_workspaceSplitter->setObjectName("workspaceSplitter");
    m_fileSplitter = new QSplitter(Qt::Horizontal, m_workspaceSplitter);
    m_fileSplitter->setObjectName("fileSplitter");

    auto *localTabs = new QTabWidget(m_fileSplitter);
    localTabs->setObjectName("localTabs");
    m_localPanel = new FilePanel(FilePanel::Mode::Local, localTabs);
    m_localPanel->setFileTreeVisible(m_settings.localFileTreeVisible);
    m_localPanel->setFileTreeVisibilityRequestedHandler([this](bool visible) {
        setLocalFileTreeVisible(visible);
    });
    m_localPanel->setLocalPathChangedHandler([this, localTabs](const QString &path) {
        const int tabIndex = localTabs->indexOf(m_localPanel);
        if (tabIndex >= 0)
        {
            localTabs->setTabText(tabIndex, localTabTitle(path));
        }
    });
    m_localPanel->setClipboardCopyRequestedHandler([](const QList<RemoteTransferItem> &items) {
        auto *mimeData = new QMimeData();
        QList<QUrl> urls;
        for (const RemoteTransferItem &item : items)
        {
            if (QFileInfo::exists(item.path))
            {
                urls.append(QUrl::fromLocalFile(item.path));
            }
        }
        if (urls.isEmpty())
        {
            delete mimeData;
            return;
        }
        mimeData->setUrls(urls);
        QApplication::clipboard()->setMimeData(mimeData);
    });
    m_localPanel->setClipboardPasteRequestedHandler([this](const QString &targetDirectory) {
        const QMimeData *mimeData = QApplication::clipboard()->mimeData();
        if (mimeData == nullptr || !mimeData->hasFormat(panel_shared::RemotePathMimeType))
        {
            return;
        }
        const QList<RemoteTransferItem> remoteItems = panel_shared::decodeRemoteTransferItems(
            mimeData->data(panel_shared::RemotePathMimeType));
        for (const RemoteTransferItem &item : remoteItems)
        {
            RemoteSession *session = item.sessionId.isEmpty()
                ? currentRemoteSession()
                : remoteSessionById(item.sessionId.toStdString());
            if (session == nullptr || !session->connected)
            {
                showWarningMessage("粘贴失败", "源远程会话已关闭或不可用。");
                continue;
            }
            if (item.isDirectory)
            {
                downloadRemotePath(*session, item.path, targetDirectory);
            }
            else
            {
                downloadRemoteFile(*session, item.path, targetDirectory);
            }
        }
    });
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
    m_localPanel->setRemoteFilesDroppedOnLocalHandler([this](const QList<RemoteTransferItem> &remoteItems, const QString &targetDirectory) {
        for (const RemoteTransferItem &item : remoteItems)
        {
            RemoteSession *session = item.sessionId.isEmpty()
                ? currentRemoteSession()
                : remoteSessionById(item.sessionId.toStdString());
            if (session == nullptr || !session->connected)
            {
                showWarningMessage("下载失败", "源远程会话已关闭或不可用。");
                continue;
            }
            if (item.isDirectory)
            {
                downloadRemotePath(*session, item.path, targetDirectory);
            }
            else
            {
                downloadRemoteFile(*session, item.path, targetDirectory);
            }
        }
    });
    localTabs->addTab(m_localPanel, localTabTitle(m_localPanel->currentPath()));

    m_remoteTabs = new QTabWidget(m_fileSplitter);
    m_remoteTabs->setObjectName("remoteTabs");
    m_remoteTabs->setTabsClosable(false);
    m_remoteTabs->tabBar()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_remoteTabs->tabBar(), &QTabBar::customContextMenuRequested, this, &MainWindow::showRemoteTabContextMenu);
    connect(m_remoteTabs, &QTabWidget::currentChanged, this, [this]() {
        RemoteSession *session = currentRemoteSession();
        m_remotePanel = session == nullptr ? dynamic_cast<FilePanel *>(m_remoteTabs->currentWidget()) : session->panel;
        updateFileTreeActionState();
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

    m_bottomTabs = new QTabWidget(m_workspaceSplitter);
    m_bottomTabs->setObjectName("bottomTabs");
    auto *transferTab = new QWidget(m_bottomTabs);
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
    m_transferTable->header()->setStretchLastSection(false);
    const QList<int> transferColumnWidths{200, 60, 140, 140, 140, 40, 140, 80, 80, 80};
    for (int column = 0; column < transferColumnWidths.size(); ++column)
    {
        m_transferTable->header()->setSectionResizeMode(column, QHeaderView::Interactive);
        m_transferTable->setColumnWidth(column, transferColumnWidths.at(column));
    }
    transferLayout->addWidget(transferToolbar);
    transferLayout->addWidget(m_transferTable, 1);

    connect(m_transferTable, &QTreeWidget::itemSelectionChanged, this, &MainWindow::updateTransferActionButtons);
    connect(m_cancelTransferButton, &QPushButton::clicked, this, &MainWindow::cancelSelectedTransferJob);
    connect(m_retryTransferButton, &QPushButton::clicked, this, &MainWindow::retrySelectedTransferJob);
    connect(m_clearFinishedTransfersButton, &QPushButton::clicked, this, &MainWindow::clearFinishedTransferJobs);

    m_logView = new QTreeWidget(m_bottomTabs);
    m_logView->setObjectName("logView");
    m_logView->setHeaderLabels({"时间", "级别", "消息"});
    m_logView->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_logView->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_logView->header()->setStretchLastSection(true);
    m_logView->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_logView, &QTreeWidget::customContextMenuRequested, this, [this](const QPoint &position) {
        QTreeWidgetItem *item = m_logView->itemAt(position);
        if (item == nullptr)
        {
            return;
        }

        m_logView->setCurrentItem(item);
        QMenu menu(m_logView);
        menu.setStyleSheet("QMenu::item { padding: 4px 24px; }");
        QAction *copyAction = menu.addAction("复制");
        connect(copyAction, &QAction::triggered, m_logView, [item]() {
            QApplication::clipboard()->setText(
                QString("%1\t%2\t%3").arg(item->text(0), item->text(1), item->text(2)));
        });
        menu.exec(m_logView->viewport()->mapToGlobal(position));
    });

    m_terminalHost = new QWidget(m_bottomTabs);
    m_terminalHost->setObjectName("terminalHost");
    auto *terminalHostLayout = new QVBoxLayout(m_terminalHost);
    terminalHostLayout->setContentsMargins(0, 0, 0, 0);
    m_terminalTabs = new QTabWidget(m_terminalHost);
    m_terminalTabs->setObjectName("terminalTabs");
    m_terminalTabs->setTabsClosable(true);
    m_terminalTabs->setMovable(true);
    terminalHostLayout->addWidget(m_terminalTabs);
    connect(
        m_terminalTabs,
        &QTabWidget::tabCloseRequested,
        this,
        &MainWindow::closeTerminalTab);

    m_bottomTabs->addTab(transferTab, "传输");
    m_bottomTabs->addTab(m_logView, "日志");
    m_bottomTabs->addTab(m_terminalHost, "终端");

    m_terminalMaximizeButton = new QToolButton(m_bottomTabs);
    m_terminalMaximizeButton->setObjectName("terminalMaximizeButton");
    m_terminalMaximizeButton->setAutoRaise(true);
    m_terminalMaximizeButton->setFocusPolicy(Qt::NoFocus);
    m_terminalMaximizeButton->setIcon(
        style()->standardIcon(QStyle::SP_TitleBarMaxButton));
    m_bottomTabs->setCornerWidget(
        m_terminalMaximizeButton, Qt::TopRightCorner);
    connect(m_terminalMaximizeButton, &QToolButton::clicked, this, [this]() {
        setTerminalWorkspaceMaximized(!m_terminalWorkspaceMaximized);
    });
    connect(m_bottomTabs, &QTabWidget::currentChanged, this, [this]() {
        if (m_bottomTabs->currentWidget() != m_terminalHost
            && m_terminalWorkspaceMaximized)
        {
            setTerminalWorkspaceMaximized(false);
            return;
        }
        updateTerminalWorkspaceControls();
    });

    m_workspaceSplitter->addWidget(m_fileSplitter);
    m_workspaceSplitter->addWidget(m_bottomTabs);
    m_workspaceSplitter->setStretchFactor(0, 5);
    m_workspaceSplitter->setStretchFactor(1, 1);

    setCentralWidget(m_workspaceSplitter);
    updateTerminalWorkspaceControls();
    updateTransferActionButtons();
}

/**
 * @brief 统一设置本地面板和所有远程会话的文件树状态。
 */
void MainWindow::setAllFileTreesVisible(bool visible)
{
    if (m_localPanel != nullptr)
    {
        m_localPanel->setFileTreeVisible(visible);
    }

    const bool localStateChanged = m_settings.localFileTreeVisible != visible;
    m_settings.localFileTreeVisible = visible;

    bool siteChanged = false;
    for (const std::unique_ptr<RemoteSession> &session : m_remoteSessions)
    {
        if (session == nullptr)
        {
            continue;
        }
        session->fileTreeVisible = visible;
        session->profile.fileTreeVisible = visible;
        if (session->panel != nullptr)
        {
            session->panel->setFileTreeVisible(visible);
        }
        const int siteIndex = siteIndexById(session->profile.id);
        if (siteIndex >= 0 && m_sites.at(siteIndex).fileTreeVisible != visible)
        {
            m_sites.at(siteIndex).fileTreeVisible = visible;
            siteChanged = true;
        }
    }
    if (siteChanged)
    {
        saveSites();
    }
    if (localStateChanged)
    {
        saveSettings();
    }
    updateFileTreeActionState();
}

/**
 * @brief 仅设置本地面板的文件树状态。
 */
void MainWindow::setLocalFileTreeVisible(bool visible)
{
    if (m_localPanel != nullptr)
    {
        m_localPanel->setFileTreeVisible(visible);
    }
    if (m_settings.localFileTreeVisible != visible)
    {
        m_settings.localFileTreeVisible = visible;
        saveSettings();
        return;
    }
    updateFileTreeActionState();
}

/**
 * @brief 仅应用并持久化指定远程会话的文件树显示状态。
 */
void MainWindow::setFileTreeVisibilityForSession(RemoteSession *session, bool visible)
{
    if (session == nullptr)
    {
        updateFileTreeActionState();
        return;
    }

    session->fileTreeVisible = visible;
    session->profile.fileTreeVisible = visible;
    if (session->panel != nullptr)
    {
        session->panel->setFileTreeVisible(visible);
    }

    const int siteIndex = siteIndexById(session->profile.id);
    if (siteIndex >= 0)
    {
        m_sites.at(siteIndex).fileTreeVisible = visible;
        saveSites();
    }
    updateFileTreeActionState();
}

/**
 * @brief 根据所有面板的文件树状态更新顶部全局开关。
 */
void MainWindow::updateFileTreeActionState()
{
    bool allVisible = m_localPanel == nullptr || m_localPanel->isFileTreeVisible();
    for (const std::unique_ptr<RemoteSession> &session : m_remoteSessions)
    {
        if (session != nullptr)
        {
            allVisible = allVisible && session->fileTreeVisible;
        }
    }
    if (m_fileTreeAction != nullptr)
    {
        const QSignalBlocker blocker(m_fileTreeAction);
        m_fileTreeAction->setChecked(allVisible);
    }
}

/**
 * @brief 在右侧中央工作区内最大化或还原终端面板。
 *
 * 左侧会话管理器是独立 Dock，不参与该布局切换。
 */
void MainWindow::setTerminalWorkspaceMaximized(bool maximized)
{
    if (m_workspaceSplitter == nullptr || m_fileSplitter == nullptr)
    {
        return;
    }

    if (maximized && !m_terminalWorkspaceMaximized)
    {
        const QList<int> currentSizes = m_workspaceSplitter->sizes();
        if (currentSizes.size() == 2 && currentSizes.front() > 0)
        {
            m_workspaceSplitterSizes = currentSizes;
        }
        m_fileSplitter->hide();
        m_workspaceSplitter->setSizes({0, std::max(1, height())});
        m_terminalWorkspaceMaximized = true;
    }
    else if (!maximized && m_terminalWorkspaceMaximized)
    {
        m_fileSplitter->show();
        m_workspaceSplitter->setSizes(m_workspaceSplitterSizes.size() == 2
                ? m_workspaceSplitterSizes
                : QList<int>({5, 1}));
        m_terminalWorkspaceMaximized = false;
    }

    updateTerminalWorkspaceControls();
}

/**
 * @brief 根据底部当前页和最大化状态刷新终端面板按钮。
 */
void MainWindow::updateTerminalWorkspaceControls()
{
    if (m_terminalMaximizeButton == nullptr || m_bottomTabs == nullptr)
    {
        return;
    }

    const bool terminalPageVisible = m_bottomTabs->currentWidget() == m_terminalHost;
    m_terminalMaximizeButton->setVisible(terminalPageVisible);
    m_terminalMaximizeButton->setIcon(style()->standardIcon(
        m_terminalWorkspaceMaximized
            ? QStyle::SP_TitleBarNormalButton
            : QStyle::SP_TitleBarMaxButton));
    m_terminalMaximizeButton->setToolTip(
        m_terminalWorkspaceMaximized ? "还原终端区域" : "最大化终端区域");
    m_terminalMaximizeButton->setAccessibleName(
        m_terminalWorkspaceMaximized ? "还原终端区域" : "最大化终端区域");
}

/**
 * @brief 根据当前远程标签页数量调整本地/远程分栏布局。
 */
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

/**
 * @brief 按当前连接态刷新菜单、按钮和快速连接栏的可操作状态。
 */
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
