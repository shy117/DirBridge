#ifndef DIRBRIDGE_UI_MAINWINDOW_H
#define DIRBRIDGE_UI_MAINWINDOW_H

#include <QMainWindow>
#include <QList>
#include <QPoint>

#include <memory>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "config/SiteProfile.h"
#include "config/SettingsStore.h"
#include "config/SiteStore.h"
#include "core/DependencyCheck.h"
#include "core/RemoteFileSystem.h"
#include "core/TransferJob.h"
#include "core/TransferQueue.h"

class QComboBox;
class QDockWidget;
class QFileInfo;
class QLabel;
class QLineEdit;
class QPushButton;
class QAction;
class QCloseEvent;
class QSplitter;
class QTabWidget;
class QThread;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class ExternalEditManager;
class FilePanel;
class TerminalWidget;
namespace dirbridge::terminal {
class SshTerminalManager;
}

class MainWindow : public QMainWindow
{
public:
    /**
     * @brief 构建主应用窗口并初始化界面状态。
     * @param dependencyCheck 启动依赖检查结果，用于展示环境状态。
     * @param parent 可选的 Qt 父部件。
     */
    explicit MainWindow(const DependencyCheckResult &dependencyCheck, QWidget *parent = nullptr);
    ~MainWindow() override;

    /**
     * @brief 为自动化 UI 测试替换远程文件系统后端。
     * @param remoteFileSystem 后续远程操作要使用的后端实例。
     */
    void setRemoteFileSystemForTesting(std::unique_ptr<RemoteFileSystem> remoteFileSystem);

    /**
     * @brief 为自动化 UI 测试触发标准的单文件上传流程。
     * @param localPath 要上传到当前远程目录的本地文件路径。
     */
    void uploadLocalFileForTesting(const QString &localPath);

    /**
     * @brief 为自动化 UI 测试触发标准的文件或目录上传流程。
     * @param localPath 要上传到当前远程目录的本地文件或目录路径。
     */
    void uploadLocalPathForTesting(const QString &localPath);

    /**
     * @brief 为自动化 UI 测试触发标准的单文件下载流程。
     * @param remotePath 要下载到当前本地目录的远程文件路径。
     */
    void downloadRemoteFileForTesting(const QString &remotePath);

    /**
     * @brief 为自动化 UI 测试触发标准的文件或目录下载流程。
     * @param remotePath 要下载到当前本地目录的远程文件或目录路径。
     */
    void downloadRemotePathForTesting(const QString &remotePath);

    /**
     * @brief 为自动化 UI 测试触发远程文件外部编辑流程。
     * @param remotePath 当前会话中的远程文件绝对路径。
     */
    void editRemoteFileForTesting(const QString &remotePath);
    void closeRemoteEditForTesting(const QString &remotePath);
    void setCurrentRemoteFileTreeVisibleForTesting(bool visible);

    /**
     * @brief 为自动化 UI 测试替换外部编辑器启动行为。
     * @param launcher 接收缓存文件路径并返回是否成功打开的回调。
     */
    void setExternalEditorLauncherForTesting(std::function<bool(const QString &)> launcher);

    /**
     * @brief 为自动化 UI 清理触发标准的远程删除流程。
     * @param path 要删除的远程文件或空目录路径。
     */
    void removeRemotePathForTesting(const QString &path);

    /**
     * @brief 为自动化 UI 测试触发远程权限修改流程。
     */
    void setRemotePermissionsForTesting(const QString &path, int mode, bool recursive = false);

    /**
     * @brief 为自动化 UI 测试触发标准的远程移动流程。
     * @param sourcePaths 待移动的远程源路径列表。
     * @param targetDirectory 目标远程目录。
     */
    void moveRemotePathsForTesting(const QStringList &sourcePaths, const QString &targetDirectory);

    /**
     * @brief 在自动化测试期间启用或禁用模态消息框。
     * @param suppressed 为 true 时抑制模态对话框。
     */
    void setDialogsSuppressedForTesting(bool suppressed);

    /**
     * @brief 在自动化测试中将本地文件面板切换到指定目录。
     * @param path 作为当前下载目标的本地目录路径。
     */
    void setLocalPathForTesting(const QString &path);

    /**
     * @brief 在自动化 UI 测试中保存或更新站点配置。
     * @param profile 要新增或按 id 替换的站点配置。
     */
    void saveSiteForTesting(const SiteProfile &profile);

    /**
     * @brief 在自动化 UI 测试中删除已保存站点。
     * @param siteId 要删除的稳定站点 id。
     * @return 删除成功时返回 true。
     */
    bool removeSiteForTesting(const std::string &siteId);

    /**
     * @brief 在自动化 UI 测试中重命名站点分组。
     * @param oldGroup 原分组名称；为空时表示未分组站点。
     * @param newGroup 新分组名称；为空时表示移动到未分组。
     * @return 至少更新了一个站点时返回 true。
     */
    bool renameSiteGroupForTesting(const QString &oldGroup, const QString &newGroup);

protected:
    void closeEvent(QCloseEvent *event) override;

private:
    struct RemoteConnectionResult;

    struct RemoteSession
    {
        QString id;
        SiteProfile profile;
        std::shared_ptr<RemoteFileSystem> fileSystem;
        FilePanel *panel = nullptr;
        QString currentPath;
        QStringList knownDirectories;
        bool connected = false;
        bool connecting = false;
        std::uint64_t directoryLoadGeneration = 0;
        int activeRemoteOperationCount = 0;
        bool fileTreeVisible = true;
        std::shared_ptr<std::atomic_bool> connectionCanceled;
        QString displayName;
    };

    struct TerminalTab
    {
        QWidget *page = nullptr;
        TerminalWidget *terminal = nullptr;
        bool active = true;
        bool closeRequested = false;
    };

    void loadSites();
    void saveSites();
    void loadSettings();
    void saveSettings();
    void setupMenuBar();
    void setupToolBar();
    void setupQuickConnectBar();
    void setupCentralWorkspace(const DependencyCheckResult &dependencyCheck);
    QTreeWidget *createSessionManager();
    void populateSessionManager();
    void showSessionManagerContextMenu(const QPoint &position);
    /**
     * @brief 为远程会话标签页安装统一风格的关闭按钮。
     * @param index 需要安装自定义关闭按钮的远程标签页索引。
     */
    void installRemoteTabCloseButton(int index);
    void closeRemoteTab(int index);
    void showRemoteTabContextMenu(const QPoint &position);
    void setupSshTerminalManager();
    void openSshTerminal(const SiteProfile &profile);
    void closeTerminalTab(int index);
    void removeTerminalTab(const QString &terminalId);
    void setTerminalWorkspaceMaximized(bool maximized);
    void updateTerminalWorkspaceControls();
    void appendLog(const QString &level, const QString &message);
    void beginBackgroundTask();
    void finishBackgroundTask();
    void cancelActiveTransfersForClose();

    /**
     * @brief 根据自动化测试的对话框设置显示或记录警告消息。
     * @param title 消息框标题或日志前缀。
     * @param message 面向用户的警告文本。
     */
    void showWarningMessage(const QString &title, const QString &message);

    /**
     * @brief 根据自动化测试的对话框设置显示或记录严重错误消息。
     * @param title 消息框标题或日志前缀。
     * @param message 面向用户的严重错误文本。
     */
    void showCriticalMessage(const QString &title, const QString &message);
    SiteProfile profileFromQuickConnect() const;
    void connectQuickProfile(bool saveProfile);
    void showRemoteProfile(const SiteProfile &profile, const QString &initialRemotePath = {});
    RemoteSession *createRemoteSession(const SiteProfile &profile, std::unique_ptr<RemoteFileSystem> fileSystem);
    RemoteSession *currentRemoteSession() const;
    RemoteSession *remoteSessionByPanel(FilePanel *panel) const;
    RemoteSession *remoteSessionById(const std::string &sessionId) const;
    /**
     * @brief 检查是否有远程会话正在执行初始连接任务。
     * @return 当前存在连接尝试时返回 true。
     */
    bool hasConnectingRemoteSession() const;
    /**
     * @brief 在后台线程启动初始远程连接和默认目录加载。
     * @param session 持有待连接状态的会话对象。
     */
    void startRemoteConnection(RemoteSession &session);
    /**
     * @brief 将后台连接结果回填到 UI 线程。
     * @param sessionId 任务启动时捕获的会话 id。
     * @param result 后台任务返回的连接结果和后端所有权。
     */
    void finishRemoteConnection(const QString &sessionId, const std::shared_ptr<RemoteConnectionResult> &result);
    /**
     * @brief 请求逻辑取消正在进行中的远程连接。
     * @param session 需要取消连接尝试的会话。
     */
    void cancelRemoteConnection(RemoteSession &session);
    void reconnectRemoteSession(RemoteSession &session);
    /**
     * @brief 按未连接、连接中和已连接状态更新菜单与快速连接控件。
     */
    void updateRemoteConnectionActions();
    bool loadRemotePath(RemoteSession &session, const QString &path, bool addToHistory, QString *errorMessage = nullptr);
    bool loadRemotePath(const QString &path, bool addToHistory, QString *errorMessage = nullptr);
    void refreshRemote();
    void disconnectRemote();
    void createRemoteDirectory(RemoteSession &session, const QString &path);
    /**
     * @brief 在已连接的远程文件系统上创建空文件。
     * @param path 要创建的远程文件绝对路径。
     */
    void createRemoteFile(RemoteSession &session, const QString &path);
    void startRemoteMutation(RemoteSession &session,
                             const QString &errorTitle,
                             const QString &successMessage,
                             std::function<RemoteOperationResult(RemoteFileSystem &)> operation);
    void removeRemotePath(RemoteSession &session, const QString &path, std::optional<FileItemType> knownType = std::nullopt);
    void removeRemotePath(const QString &path);
    void finishRemoteRemove(const QString &sessionId, const QString &path, const QString &errorMessage);
    void renameRemotePath(RemoteSession &session, const QString &sourcePath, const QString &targetPath);
    void setRemotePermissions(RemoteSession &session, const QString &path, int mode, bool recursive);
    bool removeRemotePathRecursive(RemoteSession &session,
                                   const QString &path,
                                   QString *errorMessage = nullptr,
                                   std::optional<FileItemType> knownType = std::nullopt);
    void moveRemotePaths(RemoteSession &session, const QStringList &sourcePaths, const QString &targetDirectory);
    void setRemoteConnectionState(RemoteSession &session, bool connected, const QString &message);
    void updateFileSplitterLayout();
    void setAllFileTreesVisible(bool visible);
    void setLocalFileTreeVisible(bool visible);
    void setFileTreeVisibilityForSession(RemoteSession *session, bool visible);
    void updateFileTreeActionState();
    /**
     * @brief 将传输任务加入队列并刷新传输表格。
     * @param job 待显示和调度的传输任务。
     */
    void enqueueTransferJob(const TransferJob &job);
    /**
     * @brief 根据子文件任务状态更新目录传输的聚合行。
     */
    void updateDirectoryTransferParents();
    void refreshPreparingTransfer(const QString &parentJobId);
    /**
     * @brief 为目录传输创建可见的父任务行。
     * @param direction 上传或下载方向。
     * @param name 目录显示名称。
     * @param localPath 本地目录根路径。
     * @param remotePath 远程目录根路径。
     * @param session 当前远程会话。
     * @return 生成的父任务传输 id。
     */
    QString enqueueDirectoryTransferParent(TransferDirection direction, const QString &name, const QString &localPath, const QString &remotePath, const RemoteSession &session);
    /**
     * @brief 通过核心传输管理器执行等待中的传输任务。
     */
    void processTransferQueue();
    void handleTransferProgress(const QString &jobId, std::int64_t transferredBytes, std::int64_t totalBytes);
    void handleTransferFinished(const QString &jobId, const RemoteOperationResult &result, bool canceled);
    bool hasRunningTransferForSession(const QString &sessionId) const;
    void refreshTransferTable();
    void updateTransferActionButtons();
    QString selectedTransferJobId() const;
    void cancelSelectedTransferJob();
    void retrySelectedTransferJob();
    void clearFinishedTransferJobs();
    void showAboutDialog();
    void uploadLocalFile(RemoteSession &session, const QString &localPath);
    void uploadLocalFile(const QString &localPath);
    void startSingleFileUploadPreparation(RemoteSession &session, const QString &jobId);
    void finishSingleFileUploadPreparation(const QString &jobId, bool exists, const FileItem &existingItem, const QString &errorMessage);
    void uploadLocalPath(RemoteSession &session, const QString &localPath);
    void startLocalDirectoryUploadPreparation(RemoteSession &session, const QString &localDirectoryPath, const QString &remoteDirectoryPath, const QString &parentJobId);
    enum class UploadConflictAction
    {
        Overwrite,
        Skip,
        ContinueUpload,
        Rename,
        Cancel
    };
    bool remotePathExists(RemoteSession &session, const QString &remotePath, FileItem *item = nullptr);
    UploadConflictAction chooseUploadConflictAction(const QFileInfo &localInfo, const QString &remotePath, const FileItem &remoteItem) const;
    QString renamedRemotePathForUpload(const QString &remotePath, const QFileInfo &localInfo) const;
    void downloadRemoteFile(RemoteSession &session, const QString &remotePath);
    void downloadRemoteFile(const QString &remotePath);
    void downloadRemotePath(RemoteSession &session, const QString &remotePath);
    void startRemoteDirectoryDownloadPreparation(RemoteSession &session, const QString &remoteDirectoryPath, const QString &localDirectoryPath, const QString &parentJobId);
    void appendPreparedDirectoryTransferJobs(const QString &parentJobId, const std::vector<TransferJob> &jobs);
    void finishPreparedDirectoryTransfer(const QString &parentJobId, const QString &errorMessage);
    void handlePreparedDirectoryTransfer(const QString &parentJobId, const std::vector<TransferJob> &jobs, const QString &errorMessage);
    bool enqueueRemoteDirectoryDownload(RemoteSession &session, const QString &remoteDirectoryPath, const QString &localDirectoryPath, const QString &parentJobId = {}, QString *errorMessage = nullptr);
    int siteIndexById(const std::string &siteId) const;
    void connectSiteAtIndex(int index, const QString &initialRemotePath = {});
    void editSiteAtIndex(int index);
    void deleteSiteAtIndex(int index);
    bool renameSiteGroup(const QString &oldGroup, const QString &newGroup);
    void promptRenameSiteGroup(const QString &oldGroup);
    void recordRecentSession(const RemoteSession &session);
    void connectRecentSession(const std::string &siteId, const QString &lastRemotePath);
    void fillQuickConnectFromItem(QTreeWidgetItem *item);
    QString siteDisplayName(const SiteProfile &profile) const;

private:
    SiteStore m_siteStore;
    SettingsStore m_settingsStore;
    std::vector<SiteProfile> m_sites;
    UserSettings m_settings;
    std::unique_ptr<RemoteFileSystem> m_testingRemoteFileSystem;
    std::unique_ptr<ExternalEditManager> m_externalEditManager;
    std::unique_ptr<dirbridge::terminal::SshTerminalManager> m_sshTerminalManager;
    bool m_dialogsSuppressedForTesting = false;
    std::vector<std::unique_ptr<RemoteSession>> m_remoteSessions;
    TransferQueue m_transferQueue;
    int m_activeBackgroundTaskCount = 0;
    bool m_closePending = false;
    bool m_closeReady = false;
    bool m_transferWorkerRunning = false;
    QString m_runningTransferJobId;
    QThread *m_transferThread = nullptr;
    std::map<std::string, std::shared_ptr<std::atomic_bool>> m_transferCancelFlags;
    std::set<QString> m_pendingRemoteDeletes;
    std::map<QString, TerminalTab> m_terminalUiSessions;

    QAction *m_disconnectAction = nullptr;
    QAction *m_refreshAction = nullptr;
    QAction *m_fileTreeAction = nullptr;
    QComboBox *m_protocolCombo = nullptr;
    QLineEdit *m_hostEdit = nullptr;
    QLineEdit *m_portEdit = nullptr;
    QLineEdit *m_userEdit = nullptr;
    QLineEdit *m_passwordEdit = nullptr;
    QLineEdit *m_remotePathEdit = nullptr;
    QPushButton *m_connectButton = nullptr;
    QPushButton *m_saveSiteButton = nullptr;
    QTreeWidget *m_sessionTree = nullptr;
    QDockWidget *m_sessionDock = nullptr;
    QTreeWidget *m_transferTable = nullptr;
    QPushButton *m_cancelTransferButton = nullptr;
    QPushButton *m_retryTransferButton = nullptr;
    QPushButton *m_clearFinishedTransfersButton = nullptr;
    QTreeWidget *m_logView = nullptr;
    QTabWidget *m_bottomTabs = nullptr;
    QTabWidget *m_terminalTabs = nullptr;
    QWidget *m_terminalHost = nullptr;
    QToolButton *m_terminalMaximizeButton = nullptr;
    FilePanel *m_localPanel = nullptr;
    QSplitter *m_workspaceSplitter = nullptr;
    QSplitter *m_fileSplitter = nullptr;
    QTabWidget *m_remoteTabs = nullptr;
    FilePanel *m_remotePanel = nullptr;
    QList<int> m_workspaceSplitterSizes;
    bool m_terminalWorkspaceMaximized = false;
};

#endif // DIRBRIDGE_UI_MAINWINDOW_H
