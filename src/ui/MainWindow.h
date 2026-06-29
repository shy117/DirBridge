#ifndef DIRBRIDGE_UI_MAINWINDOW_H
#define DIRBRIDGE_UI_MAINWINDOW_H

#include <QMainWindow>
#include <QPoint>

#include <memory>
#include <atomic>
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
class QLineEdit;
class QPushButton;
class QAction;
class QTabWidget;
class QTreeWidget;
class QTreeWidgetItem;
class FilePanel;

class MainWindow : public QMainWindow
{
public:
    /**
     * @brief Constructs the main application window and initializes UI state.
     * @param dependencyCheck Startup dependency check result used to show environment status.
     * @param parent Optional Qt parent widget.
     */
    explicit MainWindow(const DependencyCheckResult &dependencyCheck, QWidget *parent = nullptr);

    /**
     * @brief Replaces the remote filesystem backend for automated UI tests.
     * @param remoteFileSystem Backend instance to use for future remote operations.
     */
    void setRemoteFileSystemForTesting(std::unique_ptr<RemoteFileSystem> remoteFileSystem);

    /**
     * @brief Invokes the normal single-file upload workflow for automated UI tests.
     * @param localPath Local file path to upload into the current remote directory.
     */
    void uploadLocalFileForTesting(const QString &localPath);

    /**
     * @brief Invokes the normal file-or-directory upload workflow for automated UI tests.
     * @param localPath Local file or directory path to upload into the current remote directory.
     */
    void uploadLocalPathForTesting(const QString &localPath);

    /**
     * @brief Invokes the normal single-file download workflow for automated UI tests.
     * @param remotePath Remote file path to download into the current local directory.
     */
    void downloadRemoteFileForTesting(const QString &remotePath);

    /**
     * @brief Invokes the normal file-or-directory download workflow for automated UI tests.
     * @param remotePath Remote file or directory path to download into the current local directory.
     */
    void downloadRemotePathForTesting(const QString &remotePath);

    /**
     * @brief Invokes the normal remote remove workflow for automated UI cleanup.
     * @param path Remote file or empty directory path to remove.
     */
    void removeRemotePathForTesting(const QString &path);

    /**
     * @brief Invokes the normal remote move workflow for automated UI tests.
     * @param sourcePaths Source remote paths to move.
     * @param targetDirectory Target remote directory.
     */
    void moveRemotePathsForTesting(const QStringList &sourcePaths, const QString &targetDirectory);

    /**
     * @brief Enables or disables modal message boxes during automated tests.
     * @param suppressed true to suppress modal dialogs.
     */
    void setDialogsSuppressedForTesting(bool suppressed);

    /**
     * @brief Navigates the local file panel to a directory for automated tests.
     * @param path Local directory path to use as the current download target.
     */
    void setLocalPathForTesting(const QString &path);

    /**
     * @brief Saves or updates a site profile during automated UI tests.
     * @param profile Site profile to add or replace by id.
     */
    void saveSiteForTesting(const SiteProfile &profile);

    /**
     * @brief Removes a saved site during automated UI tests.
     * @param siteId Stable site id to remove.
     * @return true when a site was removed.
     */
    bool removeSiteForTesting(const std::string &siteId);

    /**
     * @brief Renames a site group during automated UI tests.
     * @param oldGroup Previous group name, or empty for ungrouped sites.
     * @param newGroup New group name, or empty to move sites to ungrouped.
     * @return true when at least one site was updated.
     */
    bool renameSiteGroupForTesting(const QString &oldGroup, const QString &newGroup);

private:
    struct RemoteConnectionResult;

    struct RemoteSession
    {
        QString id;
        SiteProfile profile;
        std::unique_ptr<RemoteFileSystem> fileSystem;
        FilePanel *panel = nullptr;
        QString currentPath;
        bool connected = false;
        bool connecting = false;
        std::shared_ptr<std::atomic_bool> connectionCanceled;
        QString displayName;
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
     * @brief Installs the neutral close button used by remote session tabs.
     * @param index Remote tab index that should receive the custom close button.
     */
    void installRemoteTabCloseButton(int index);
    void closeRemoteTab(int index);
    void appendLog(const QString &level, const QString &message);

    /**
     * @brief Shows or logs a warning message depending on automated-test dialog settings.
     * @param title Message box title or log prefix.
     * @param message User-facing warning text.
     */
    void showWarningMessage(const QString &title, const QString &message);

    /**
     * @brief Shows or logs a critical message depending on automated-test dialog settings.
     * @param title Message box title or log prefix.
     * @param message User-facing critical error text.
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
     * @brief Checks whether any remote session is currently running its initial connection task.
     * @return true when a connection attempt is in progress.
     */
    bool hasConnectingRemoteSession() const;
    /**
     * @brief Starts the initial remote connection and default-directory load on a background thread.
     * @param session Session that owns the pending connection state.
     */
    void startRemoteConnection(RemoteSession &session);
    /**
     * @brief Applies the background connection result back on the UI thread.
     * @param sessionId Session id captured when the task started.
     * @param result Connection result and backend ownership returned by the worker.
     */
    void finishRemoteConnection(const QString &sessionId, const std::shared_ptr<RemoteConnectionResult> &result);
    /**
     * @brief Requests logical cancellation of an in-flight remote connection.
     * @param session Session whose connection attempt should be canceled.
     */
    void cancelRemoteConnection(RemoteSession &session);
    /**
     * @brief Updates menu and quick-connect controls for disconnected, connecting, and connected states.
     */
    void updateRemoteConnectionActions();
    bool loadRemotePath(RemoteSession &session, const QString &path, bool addToHistory, QString *errorMessage = nullptr);
    bool loadRemotePath(const QString &path, bool addToHistory, QString *errorMessage = nullptr);
    void refreshRemote();
    void disconnectRemote();
    void createRemoteDirectory(RemoteSession &session, const QString &path);
    /**
     * @brief Creates an empty file on the connected remote filesystem.
     * @param path Absolute remote file path to create.
     */
    void createRemoteFile(RemoteSession &session, const QString &path);
    void removeRemotePath(RemoteSession &session, const QString &path);
    void removeRemotePath(const QString &path);
    void renameRemotePath(RemoteSession &session, const QString &sourcePath, const QString &targetPath);
    bool removeRemotePathRecursive(RemoteSession &session, const QString &path, QString *errorMessage = nullptr);
    void moveRemotePaths(RemoteSession &session, const QStringList &sourcePaths, const QString &targetDirectory);
    void setRemoteConnectionState(RemoteSession &session, bool connected, const QString &message);
    /**
     * @brief Adds a transfer job to the queue and refreshes the transfer table.
     * @param job Pending transfer job to display and schedule.
     */
    void enqueueTransferJob(const TransferJob &job);
    /**
     * @brief Updates aggregate directory transfer rows from their child file jobs.
     */
    void updateDirectoryTransferParents();
    /**
     * @brief Creates a visible parent row for a directory transfer.
     * @param direction Upload or download direction.
     * @param name Directory display name.
     * @param localPath Local directory root.
     * @param remotePath Remote directory root.
     * @param session Current remote session.
     * @return Generated parent transfer id.
     */
    QString enqueueDirectoryTransferParent(TransferDirection direction, const QString &name, const QString &localPath, const QString &remotePath, const RemoteSession &session);
    /**
     * @brief Runs pending transfer jobs through the core transfer manager.
     */
    void processTransferQueue();
    void refreshTransferTable();
    void updateTransferActionButtons();
    QString selectedTransferJobId() const;
    void cancelSelectedTransferJob();
    void retrySelectedTransferJob();
    void clearFinishedTransferJobs();
    void showAboutDialog();
    void uploadLocalFile(RemoteSession &session, const QString &localPath);
    void uploadLocalFile(const QString &localPath);
    void uploadLocalPath(RemoteSession &session, const QString &localPath);
    bool enqueueLocalDirectoryUpload(RemoteSession &session, const QString &localDirectoryPath, const QString &remoteDirectoryPath, const QString &parentJobId = {}, QString *errorMessage = nullptr);
    bool ensureRemoteDirectory(RemoteSession &session, const QString &remoteDirectoryPath, QString *errorMessage = nullptr);
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
    bool m_dialogsSuppressedForTesting = false;
    std::vector<std::unique_ptr<RemoteSession>> m_remoteSessions;
    TransferQueue m_transferQueue;

    QAction *m_disconnectAction = nullptr;
    QAction *m_refreshAction = nullptr;
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
    FilePanel *m_localPanel = nullptr;
    QTabWidget *m_remoteTabs = nullptr;
    FilePanel *m_remotePanel = nullptr;
};

#endif // DIRBRIDGE_UI_MAINWINDOW_H
