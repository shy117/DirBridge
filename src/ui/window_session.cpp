#include "ui/MainWindow.h"

#include "logging/AppLogger.h"
#include "ui/FilePanel.h"
#include "ui/window_shared.h"

#include <algorithm>
#include <map>

#include <QComboBox>
#include <QDateTime>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QTabBar>
#include <QTabWidget>
#include <QTreeWidget>
#include <QTreeWidgetItem>

using namespace window_shared;

/**
 * @brief 创建会话管理树，并绑定双击与右键交互。
 * @return 新建好的会话管理树控件。
 */
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

/**
 * @brief 依据当前站点、远程会话和最近连接记录刷新会话管理树。
 */
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

/**
 * @brief 根据当前节点类型弹出会话管理树右键菜单。
 * @param position 右键发生时在树控件内的坐标。
 */
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

/**
 * @brief 为远程标签页安装自定义关闭按钮。
 * @param index 需要安装关闭按钮的标签页索引。
 */
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

/**
 * @brief 关闭指定远程标签页，并在必要时同步清理会话状态。
 * @param index 需要关闭的标签页索引。
 */
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

/**
 * @brief 为远程标签页弹出重连、断开和关闭相关菜单。
 * @param position 右键发生在标签栏中的位置。
 */
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

/**
 * @brief 创建新的远程会话，并将其接入标签页与面板回调。
 * @param profile 当前会话对应的站点配置。
 * @param fileSystem 将要绑定到会话的远程文件系统实例。
 * @return 创建好的远程会话指针，失败时返回 nullptr。
 */
MainWindow::RemoteSession *MainWindow::createRemoteSession(const SiteProfile &profile, std::unique_ptr<RemoteFileSystem> fileSystem)
{
    if (m_remoteTabs == nullptr)
    {
        return nullptr;
    }

    auto session = std::make_unique<RemoteSession>();
    session->id = QString("%1-%2").arg(QString::fromStdString(profile.id)).arg(m_remoteSessions.size() + 1);
    session->profile = profile;
    session->fileSystem = std::shared_ptr<RemoteFileSystem>(std::move(fileSystem));
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

/**
 * @brief 获取当前选中的远程会话。
 * @return 当前会话指针；若当前没有远程面板则返回 nullptr。
 */
MainWindow::RemoteSession *MainWindow::currentRemoteSession() const
{
    if (m_remoteTabs == nullptr)
    {
        return nullptr;
    }

    auto *panel = dynamic_cast<FilePanel *>(m_remoteTabs->currentWidget());
    return remoteSessionByPanel(panel);
}

/**
 * @brief 根据远程面板指针查找所属会话。
 * @param panel 远程面板实例。
 * @return 匹配到的远程会话；未找到时返回 nullptr。
 */
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

/**
 * @brief 根据会话 id 查找远程会话。
 * @param sessionId 远程会话的稳定标识。
 * @return 匹配到的远程会话；未找到时返回 nullptr。
 */
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

/**
 * @brief 根据站点 id 查找其在站点列表中的索引。
 * @param siteId 站点稳定 id。
 * @return 找到则返回索引，未找到返回 -1。
 */
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

/**
 * @brief 连接指定索引对应的站点，并可覆盖初始远程路径。
 * @param index 站点列表索引。
 * @param initialRemotePath 连接后优先打开的远程目录。
 */
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

/**
 * @brief 打开站点编辑流程，可用于新增站点或编辑既有站点。
 * @param index 站点索引；传入负值时表示新增站点。
 */
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

/**
 * @brief 删除指定索引的站点，并清理其最近会话记录。
 * @param index 站点列表索引。
 */
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

/**
 * @brief 批量重命名站点分组，并同步更新当前已打开会话的分组信息。
 * @param oldGroup 原分组名称。
 * @param newGroup 新分组名称。
 * @return 发生实际变更时返回 true，否则返回 false。
 */
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

/**
 * @brief 弹出输入框，引导用户重命名站点分组。
 * @param oldGroup 待重命名的原始分组名称。
 */
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

/**
 * @brief 记录最近成功使用过的站点会话信息。
 * @param session 当前已连接并完成目录加载的会话。
 */
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

/**
 * @brief 从最近会话记录恢复连接。
 * @param siteId 最近会话引用的站点 id。
 * @param lastRemotePath 最近一次打开的远程目录。
 */
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

/**
 * @brief 用站点树中选中的站点填充快速连接栏。
 * @param item 当前选中的会话树节点。
 */
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

/**
 * @brief 生成站点在 UI 中展示的名称。
 * @param profile 目标站点配置。
 * @return 优先返回站点名称；若名称为空则回退为协议加主机。
 */
QString MainWindow::siteDisplayName(const SiteProfile &profile) const
{
    const QString name = QString::fromStdString(profile.name);
    if (!name.trimmed().isEmpty())
    {
        return name;
    }

    return QString("%1 %2").arg(protocolText(profile.protocol), QString::fromStdString(profile.host));
}
