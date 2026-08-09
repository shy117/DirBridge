#include "ui/MainWindow.h"

#include "protocol/CurlRemoteFileSystem.h"
#include "ui/FilePanel.h"
#include "ui/window_shared.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QLineEdit>
#include <QMetaObject>
#include <QPointer>
#include <QStatusBar>
#include <QStringList>
#include <QThread>
#include <QTabWidget>
#include <QMessageBox>

using namespace window_shared;

namespace
{
struct RemoteDirectorySnapshot
{
    QStringList knownDirectories;
    std::vector<FileItem> items;
};

RemoteDirectorySnapshot loadRemoteDirectorySnapshot(RemoteFileSystem &fileSystem,
                                                    const QString &requestedPath,
                                                    bool includeAncestorSiblings)
{
    const QString path = normalizedRemotePath(requestedPath);
    RemoteDirectorySnapshot snapshot;
    snapshot.items = fileSystem.listDirectory(path.toStdString());

    const QStringList ancestors = ancestorRemoteDirectories(path);
    snapshot.knownDirectories = ancestors;
    if (includeAncestorSiblings)
    {
        for (const QString &directory : ancestors)
        {
            if (normalizedRemotePath(directory) == path)
            {
                continue;
            }

            try
            {
                const std::vector<FileItem> siblingItems = fileSystem.listDirectory(directory.toStdString());
                for (const FileItem &sibling : siblingItems)
                {
                    if (sibling.type == FileItemType::Directory)
                    {
                        snapshot.knownDirectories << QString::fromStdString(sibling.path);
                    }
                }
            }
            catch (const std::exception &)
            {
            }
        }
    }
    for (const FileItem &item : snapshot.items)
    {
        if (item.type == FileItemType::Directory)
        {
            snapshot.knownDirectories << QString::fromStdString(item.path);
        }
    }
    snapshot.knownDirectories.removeDuplicates();
    return snapshot;
}
} // namespace

struct MainWindow::RemoteConnectionResult
{
    SiteProfile profile;
    QString requestedPath;
    QString finalMessage;
    QString detail;
    QStringList knownDirectories;
    std::vector<FileItem> items;
    std::shared_ptr<RemoteFileSystem> fileSystem;
    bool canceled = false;
    bool connected = false;
    bool loaded = false;
};
/**
 * @brief 从快速连接栏读取当前输入，并转换为临时站点配置。
 * @return 可用于发起连接的站点配置。
 */
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

/**
 * @brief 执行快速连接流程，并按需保存当前连接信息为站点。
 * @param saveProfile 为 true 时先写入站点列表再发起连接。
 */
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

/**
 * @brief 基于站点配置创建远程会话，并启动后台连接流程。
 * @param profile 要连接的站点配置。
 * @param initialRemotePath 可选的初始远程目录覆盖值。
 */
void MainWindow::showRemoteProfile(const SiteProfile &profile, const QString &initialRemotePath)
{
    if (m_closePending)
    {
        return;
    }

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

/**
 * @brief 在后台线程中连接远程站点，并加载默认目录。
 * @param session 承载连接状态和远程后端的会话对象。
 */
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
    session.knownDirectories.clear();
    ++session.directoryLoadGeneration;
    if (session.connectionCanceled == nullptr)
    {
        session.connectionCanceled = std::make_shared<std::atomic_bool>(false);
    }
    session.connectionCanceled->store(false);
    const QString sessionId = session.id;
    const SiteProfile profile = session.profile;
    const QString defaultRemotePath = QString::fromStdString(profile.defaultRemotePath.empty() ? "/" : profile.defaultRemotePath);
    const std::shared_ptr<RemoteFileSystem> fileSystem = session.fileSystem;

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
    beginBackgroundTask();
    std::thread([window, sessionId, profile, defaultRemotePath, canceled, fileSystem]() {
        auto result = std::make_shared<RemoteConnectionResult>();
        result->profile = profile;
        result->requestedPath = defaultRemotePath;
        result->fileSystem = fileSystem;

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
                    RemoteDirectorySnapshot snapshot = loadRemoteDirectorySnapshot(
                        *result->fileSystem,
                        defaultRemotePath,
                        true);
                    result->items = std::move(snapshot.items);
                    result->knownDirectories = std::move(snapshot.knownDirectories);
                    result->loaded = true;
                }
                catch (const std::exception &error)
                {
                    result->detail = QString::fromUtf8(error.what());
                    result->finalMessage = QString("站点连接未完成，无法加载默认目录“%1”。%2")
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

/**
 * @brief 将后台连接结果应用回 UI 线程。
 * @param sessionId 后台任务启动时捕获的会话 id。
 * @param result 后台连接和目录加载结果。
 */
void MainWindow::finishRemoteConnection(const QString &sessionId, const std::shared_ptr<RemoteConnectionResult> &result)
{
    finishBackgroundTask();
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

    session->fileSystem = result->fileSystem;
    session->connected = true;
    session->currentPath = result->requestedPath;
    session->knownDirectories = result->knownDirectories;
    session->panel->setRemoteKnownDirectories(session->knownDirectories);
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

/**
 * @brief 请求取消正在进行的远程连接。
 * @param session 需要取消连接的远程会话。
 */
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

/**
 * @brief 断开并重新创建远程后端，然后再次发起连接。
 * @param session 需要重连的远程会话。
 */
void MainWindow::reconnectRemoteSession(RemoteSession &session)
{
    if (session.connecting)
    {
        return;
    }
    if (hasRunningTransferForSession(session.id))
    {
        showWarningMessage("无法重连会话", "当前远程会话仍有后台操作或传输任务正在运行，请等待完成或先取消传输。");
        return;
    }
    if (session.fileSystem != nullptr && session.connected)
    {
        session.fileSystem->disconnect();
    }

    session.fileSystem = m_testingRemoteFileSystem != nullptr
        ? std::shared_ptr<RemoteFileSystem>(std::move(m_testingRemoteFileSystem))
        : std::make_shared<CurlRemoteFileSystem>();
    session.currentPath.clear();
    session.connected = false;
    appendLog("INFO", QString("正在重连远程会话：%1").arg(session.displayName));
    startRemoteConnection(session);
}

/**
 * @brief 加载指定远程目录，并刷新当前会话面板。
 * @param session 目标远程会话。
 * @param path 需要加载的远程目录。
 * @param addToHistory 是否写入面板导航历史。
 * @param errorMessage 可选错误输出。
 * @return 请求成功提交到后台线程时返回 true。
 */
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

    const QString normalizedPath = normalizedRemotePath(path);
    const QString sessionId = session.id;
    const std::shared_ptr<RemoteFileSystem> fileSystem = session.fileSystem;
    const std::uint64_t requestGeneration = ++session.directoryLoadGeneration;
    ++session.activeRemoteOperationCount;
    QPointer<MainWindow> window(this);
    auto snapshot = std::make_shared<RemoteDirectorySnapshot>();
    auto detail = std::make_shared<QString>();

    if (errorMessage != nullptr)
    {
        errorMessage->clear();
    }
    session.panel->setRemoteLoading(normalizedPath);
    statusBar()->showMessage(QString("正在加载远程目录：%1").arg(normalizedPath));

    QThread *thread = QThread::create([window, sessionId, normalizedPath, addToHistory, requestGeneration, fileSystem, snapshot, detail]() {
        try
        {
            *snapshot = loadRemoteDirectorySnapshot(*fileSystem, normalizedPath, false);
        }
        catch (const std::exception &error)
        {
            *detail = QString::fromUtf8(error.what());
        }

        if (window == nullptr)
        {
            return;
        }

        QMetaObject::invokeMethod(window.data(), [window, sessionId, normalizedPath, addToHistory, requestGeneration, snapshot, detail]() {
            if (window == nullptr)
            {
                return;
            }

            RemoteSession *currentSession = window->remoteSessionById(sessionId.toStdString());
            if (currentSession == nullptr)
            {
                return;
            }
            currentSession->activeRemoteOperationCount = std::max(0, currentSession->activeRemoteOperationCount - 1);
            if (!currentSession->connected
                || currentSession->directoryLoadGeneration != requestGeneration)
            {
                return;
            }

            if (!detail->isEmpty())
            {
                const QString message = QString("无法加载远程目录“%1”。%2")
                    .arg(normalizedPath, userFacingRemoteError(*detail));
                window->appendLog("ERROR", QString("远程目录加载失败：%1").arg(*detail));
                currentSession->panel->setRemoteError(message);
                if (!window->m_dialogsSuppressedForTesting)
                {
                    window->showWarningMessage("远程目录加载失败", message);
                }
                return;
            }

            currentSession->currentPath = normalizedPath;
            const QString descendantPrefix = normalizedPath == "/" ? "/" : normalizedPath + "/";
            for (int index = currentSession->knownDirectories.size() - 1; index >= 0; --index)
            {
                if (currentSession->knownDirectories.at(index).startsWith(descendantPrefix))
                {
                    currentSession->knownDirectories.removeAt(index);
                }
            }
            currentSession->knownDirectories.append(snapshot->knownDirectories);
            currentSession->knownDirectories.removeDuplicates();
            currentSession->panel->setRemoteKnownDirectories(currentSession->knownDirectories);
            currentSession->panel->setRemoteItems(
                currentSession->currentPath,
                snapshot->items,
                QString("%1 个项目").arg(snapshot->items.size()),
                addToHistory);
            window->appendLog("INFO", QString("远程目录已加载：%1").arg(currentSession->currentPath));
            window->statusBar()->showMessage(QString("远程已连接：%1 %2")
                .arg(protocolText(currentSession->profile.protocol), currentSession->currentPath));
            window->recordRecentSession(*currentSession);
        }, Qt::QueuedConnection);
    });
    beginBackgroundTask();
    connect(thread, &QThread::finished, this, [this]() {
        finishBackgroundTask();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
    return true;
}

/**
 * @brief 在当前远程会话中加载指定目录。
 * @param path 需要加载的远程目录。
 * @param addToHistory 是否写入面板导航历史。
 * @param errorMessage 可选错误输出。
 * @return 请求成功提交到后台线程时返回 true。
 */
bool MainWindow::loadRemotePath(const QString &path, bool addToHistory, QString *errorMessage)
{
    RemoteSession *session = currentRemoteSession();
    if (session == nullptr)
    {
        return false;
    }
    return loadRemotePath(*session, path, addToHistory, errorMessage);
}

/**
 * @brief 刷新当前远程会话目录；未连接时刷新本地面板。
 */
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

/**
 * @brief 断开当前远程会话，连接中则转为取消连接。
 */
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
        showWarningMessage("无法断开会话", "当前远程会话仍有后台操作或传输任务正在运行，请等待完成或先取消传输。");
        return;
    }

    ++session->directoryLoadGeneration;
    session->knownDirectories.clear();
    session->fileSystem->disconnect();
    setRemoteConnectionState(*session, false, "远程会话已断开。");
    appendLog("INFO", "远程会话已断开");
}

/**
 * @brief 在远程会话中创建目录，并刷新当前目录。
 * @param session 目标远程会话。
 * @param path 要创建的远程目录路径。
 */
void MainWindow::createRemoteDirectory(RemoteSession &session, const QString &path)
{
    if (!session.connected)
    {
        session.panel->setRemoteError("远程会话未连接，无法新建目录。");
        return;
    }

    startRemoteMutation(
        session,
        "远程新建目录失败",
        QString("远程目录已创建：%1").arg(path),
        [path](RemoteFileSystem &fileSystem) {
            return fileSystem.createDirectory(path.toStdString());
        },
        [path](RemoteSession &currentSession) {
            if (currentSession.panel != nullptr)
            {
                currentSession.panel->queueInlineRenameForPath(path);
            }
        });
}

/**
 * @brief 在远程会话中创建空文件，并刷新当前目录。
 * @param session 目标远程会话。
 * @param path 要创建的远程文件路径。
 */
void MainWindow::createRemoteFile(RemoteSession &session, const QString &path)
{
    if (!session.connected)
    {
        session.panel->setRemoteError("远程会话未连接，无法新建文件。");
        return;
    }

    startRemoteMutation(
        session,
        "远程新建文件失败",
        QString("远程文件已创建：%1").arg(path),
        [path](RemoteFileSystem &fileSystem) {
            return fileSystem.createFile(path.toStdString());
        },
        [path](RemoteSession &currentSession) {
            if (currentSession.panel != nullptr)
            {
                currentSession.panel->queueInlineRenameForPath(path);
            }
        });
}

/**
 * @brief 在后台执行单个远程变更，并统一处理中文提示和目录刷新。
 */
void MainWindow::startRemoteMutation(
    RemoteSession &session,
    const QString &errorTitle,
    const QString &successMessage,
    std::function<RemoteOperationResult(RemoteFileSystem &)> operation,
    std::function<void(RemoteSession &)> successHandler)
{
    if (!session.connected || session.fileSystem == nullptr || !operation)
    {
        showWarningMessage(errorTitle, "远程会话不可用。");
        return;
    }

    const QString sessionId = session.id;
    const std::shared_ptr<RemoteFileSystem> fileSystem = session.fileSystem;
    QPointer<MainWindow> window(this);
    ++session.activeRemoteOperationCount;

    QThread *thread = QThread::create([
        window,
        sessionId,
        fileSystem,
        errorTitle,
        successMessage,
        successHandler,
        operation = std::move(operation)]() mutable {
        const RemoteOperationResult result = operation(*fileSystem);
        if (window == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(window.data(), [window, sessionId, errorTitle, successMessage, successHandler, result]() {
            if (window == nullptr)
            {
                return;
            }
            RemoteSession *currentSession = window->remoteSessionById(sessionId.toStdString());
            if (currentSession == nullptr)
            {
                return;
            }
            currentSession->activeRemoteOperationCount = std::max(0, currentSession->activeRemoteOperationCount - 1);
            if (!result.success)
            {
                const QString detail = QString::fromStdString(result.message);
                window->appendLog("ERROR", QString("%1：%2").arg(errorTitle, detail));
                window->showWarningMessage(errorTitle, userFacingRemoteError(detail));
                return;
            }

            window->appendLog("INFO", successMessage);
            if (successHandler)
            {
                successHandler(*currentSession);
            }
            if (currentSession->connected && !currentSession->currentPath.isEmpty())
            {
                window->loadRemotePath(*currentSession, currentSession->currentPath, false);
            }
        }, Qt::QueuedConnection);
    });
    beginBackgroundTask();
    connect(thread, &QThread::finished, this, [this]() {
        finishBackgroundTask();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

/**
 * @brief 异步删除远程文件或目录。
 * @param session 目标远程会话。
 * @param path 要删除的远程路径。
 */
void MainWindow::removeRemotePath(RemoteSession &session, const QString &path, std::optional<FileItemType> knownType)
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
    const std::shared_ptr<RemoteFileSystem> fileSystem = session.fileSystem;
    const QString sessionId = session.id;
    QPointer<MainWindow> window(this);
    QThread *thread = QThread::create([window, fileSystem, sessionId, path, knownType]() {
        QString errorMessage;
        std::function<bool(const QString &, std::optional<FileItemType>)> removeRecursive;
        removeRecursive = [&](const QString &currentPath, std::optional<FileItemType> knownType) {
            if (currentPath.trimmed().isEmpty() || currentPath == "/")
            {
                errorMessage = "不允许删除远程根目录。";
                return false;
            }

            bool isDirectory = knownType == FileItemType::Directory;
            if (!knownType.has_value() || isDirectory)
            {
                try
                {
                    const std::vector<FileItem> children = fileSystem->listDirectory(currentPath.toStdString());
                    isDirectory = true;
                    for (const FileItem &child : children)
                    {
                        if (!removeRecursive(QString::fromStdString(child.path), child.type))
                        {
                            return false;
                        }
                    }
                }
                catch (const std::exception &error)
                {
                    if (knownType == FileItemType::Directory)
                    {
                        errorMessage = QString("读取目录“%1”失败：%2").arg(currentPath, QString::fromUtf8(error.what()));
                        return false;
                    }
                }
            }

            const RemoteOperationResult result = isDirectory
                ? fileSystem->removeDirectory(currentPath.toStdString())
                : fileSystem->removeFile(currentPath.toStdString());
            if (!result.success)
            {
                errorMessage = QString::fromStdString(result.message);
                return false;
            }
            return true;
        };

        removeRecursive(path, knownType);
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
    beginBackgroundTask();
    connect(thread, &QThread::finished, this, [this]() {
        finishBackgroundTask();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

/**
 * @brief 在当前远程会话中删除指定路径。
 * @param path 要删除的远程路径。
 */
void MainWindow::removeRemotePath(const QString &path)
{
    RemoteSession *session = currentRemoteSession();
    if (session != nullptr)
    {
        removeRemotePath(*session, path);
    }
}

/**
 * @brief 重命名远程文件或目录，并刷新当前目录。
 * @param session 目标远程会话。
 * @param sourcePath 原远程路径。
 * @param targetPath 目标远程路径。
 */
void MainWindow::renameRemotePath(RemoteSession &session, const QString &sourcePath, const QString &targetPath)
{
    if (!session.connected)
    {
        session.panel->setRemoteError("远程会话未连接，无法重命名。");
        return;
    }

    startRemoteMutation(
        session,
        "远程重命名失败",
        QString("远程项目已重命名：%1 -> %2").arg(sourcePath, targetPath),
        [sourcePath, targetPath](RemoteFileSystem &fileSystem) {
            return fileSystem.rename(sourcePath.toStdString(), targetPath.toStdString());
        });
}

/**
 * @brief 在后台修改远程项目权限，并可递归应用到目录内容。
 * @param session 目标远程会话。
 * @param path 远程文件或目录路径。
 * @param mode 000 到 777 的 UNIX 权限模式。
 * @param recursive 是否递归应用到目录内容。
 */
void MainWindow::setRemotePermissions(RemoteSession &session, const QString &path, int mode, bool recursive)
{
    if (!session.connected || session.fileSystem == nullptr)
    {
        showWarningMessage("更改权限失败", "远程会话未连接。");
        return;
    }
    if (mode < 0 || mode > 0777)
    {
        showWarningMessage("更改权限失败", "权限值必须位于 000 到 777 之间。");
        return;
    }

    const QString sessionId = session.id;
    const std::shared_ptr<RemoteFileSystem> fileSystem = session.fileSystem;
    auto errorMessage = std::make_shared<QString>();
    QPointer<MainWindow> window(this);
    ++session.activeRemoteOperationCount;

    QThread *thread = QThread::create([window, sessionId, fileSystem, path, mode, recursive, errorMessage]() {
        std::vector<QString> targets;
        std::function<bool(const QString &, bool)> collectTargets;
        collectTargets = [&](const QString &currentPath, bool includeChildren) {
            targets.push_back(currentPath);
            if (!includeChildren)
            {
                return true;
            }

            std::vector<FileItem> children;
            try
            {
                children = fileSystem->listDirectory(currentPath.toStdString());
            }
            catch (const std::exception &error)
            {
                *errorMessage = QString("读取目录“%1”失败：%2").arg(currentPath, QString::fromUtf8(error.what()));
                return false;
            }

            for (const FileItem &child : children)
            {
                if (child.type == FileItemType::Symlink)
                {
                    continue;
                }
                const QString childPath = QString::fromStdString(child.path);
                if (!collectTargets(childPath, child.type == FileItemType::Directory))
                {
                    return false;
                }
            }
            return true;
        };

        if (collectTargets(path, recursive))
        {
            for (auto target = targets.crbegin(); target != targets.crend(); ++target)
            {
                const RemoteOperationResult result = fileSystem->setPermissions(target->toStdString(), mode);
                if (!result.success)
                {
                    *errorMessage = QString("修改“%1”失败：%2")
                        .arg(*target, QString::fromStdString(result.message));
                    break;
                }
            }
        }

        if (window == nullptr)
        {
            return;
        }
        QMetaObject::invokeMethod(window.data(), [window, sessionId, path, mode, recursive, errorMessage]() {
            if (window == nullptr)
            {
                return;
            }
            RemoteSession *currentSession = window->remoteSessionById(sessionId.toStdString());
            if (currentSession == nullptr)
            {
                return;
            }
            currentSession->activeRemoteOperationCount = std::max(0, currentSession->activeRemoteOperationCount - 1);
            if (!errorMessage->isEmpty())
            {
                window->appendLog("ERROR", QString("远程权限修改失败：%1").arg(*errorMessage));
                window->showWarningMessage("更改权限失败", userFacingRemoteError(*errorMessage));
                return;
            }

            window->appendLog("INFO", QString("远程权限已修改：%1 -> %2%3")
                .arg(path, QString("%1").arg(mode, 3, 8, QChar('0')), recursive ? "（包含子目录）" : QString()));
            window->loadRemotePath(*currentSession, currentSession->currentPath, false);
        }, Qt::QueuedConnection);
    });
    beginBackgroundTask();
    connect(thread, &QThread::finished, this, [this]() {
        finishBackgroundTask();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

/**
 * @brief 递归删除远程路径，供覆盖上传等同步流程复用。
 * @param session 目标远程会话。
 * @param path 要删除的远程路径。
 * @param errorMessage 可选错误输出。
 * @return 删除成功返回 true。
 */
bool MainWindow::removeRemotePathRecursive(
    RemoteSession &session,
    const QString &path,
    QString *errorMessage,
    std::optional<FileItemType> knownType)
{
    if (path.trimmed().isEmpty() || path == "/")
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "不允许删除远程根目录。";
        }
        return false;
    }

    std::function<bool(const QString &, std::optional<FileItemType>)> removeRecursive;
    removeRecursive = [&](const QString &currentPath, std::optional<FileItemType> knownType) {
        bool isDirectory = knownType == FileItemType::Directory;
        if (!knownType.has_value() || isDirectory)
        {
            try
            {
                const std::vector<FileItem> children = session.fileSystem->listDirectory(currentPath.toStdString());
                isDirectory = true;
                for (const FileItem &child : children)
                {
                    if (!removeRecursive(QString::fromStdString(child.path), child.type))
                    {
                        return false;
                    }
                }
            }
            catch (const std::exception &error)
            {
                if (knownType == FileItemType::Directory)
                {
                    if (errorMessage != nullptr)
                    {
                        *errorMessage = QString("读取目录“%1”失败：%2").arg(currentPath, QString::fromUtf8(error.what()));
                    }
                    return false;
                }
            }
        }

        const RemoteOperationResult result = isDirectory
            ? session.fileSystem->removeDirectory(currentPath.toStdString())
            : session.fileSystem->removeFile(currentPath.toStdString());
        if (!result.success)
        {
            if (errorMessage != nullptr)
            {
                *errorMessage = QString::fromStdString(result.message);
            }
            return false;
        }
        return true;
    };

    return removeRecursive(path, knownType);
}

/**
 * @brief 将一组远程项目移动到目标远程目录。
 * @param session 目标远程会话。
 * @param sourcePaths 待移动的远程路径列表。
 * @param targetDirectory 目标远程目录。
 */
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
            const QString detail = QString::fromStdString(result.message);
            appendLog("ERROR", QString("远程移动失败：%1").arg(detail));
            showWarningMessage("远程移动失败", userFacingRemoteError(detail));
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

/**
 * @brief 统一更新远程会话连接状态、标签页标题和状态栏信息。
 * @param session 目标远程会话。
 * @param connected 当前是否已连接。
 * @param message 展示给用户的状态文本。
 */
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

/**
 * @brief 处理异步远程删除任务完成后的 UI 更新。
 * @param sessionId 删除任务所属会话 id。
 * @param path 被删除的远程路径。
 * @param errorMessage 删除失败时的错误信息。
 */
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
        appendLog("ERROR", QString("远程删除失败：%1").arg(errorMessage));
        showWarningMessage("远程删除失败", userFacingRemoteError(errorMessage));
        return;
    }

    appendLog("INFO", QString("远程项目已删除：%1").arg(path));
    loadRemotePath(*session, session->currentPath, false);
}
