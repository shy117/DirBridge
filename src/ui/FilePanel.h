#ifndef DIRBRIDGE_UI_FILEPANEL_H
#define DIRBRIDGE_UI_FILEPANEL_H

#include <QFileIconProvider>
#include <QList>
#include <QPoint>
#include <QSet>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <vector>

#include "core/FileItem.h"

class QLabel;
class QLineEdit;
class QEvent;
class QMimeData;
class QPushButton;
class QSplitter;
class QTableWidget;
class QTableWidgetItem;
class QTreeWidget;

struct RemoteTransferItem
{
    QString path;
    bool isDirectory = false;
};

class FilePanel : public QWidget
{
public:
    enum class Mode
    {
        Local,
        RemotePlaceholder
    };

    explicit FilePanel(Mode mode, QWidget *parent = nullptr);

    void setFileTreeVisible(bool visible);
    bool isFileTreeVisible() const;
    void setFileTreeVisibilityRequestedHandler(std::function<void(bool)> handler);
    void refresh();
    void setRemotePathRequestedHandler(std::function<void(const QString &, bool)> handler);
    void setRemoteRefreshRequestedHandler(std::function<void()> handler);
    void setRemoteCreateDirectoryRequestedHandler(std::function<void(const QString &)> handler);
    /**
     * @brief 设置远程右键菜单请求新建空文件时使用的回调。
     * @param handler 接收远程文件绝对路径的回调。
     */
    void setRemoteCreateFileRequestedHandler(std::function<void(const QString &)> handler);
    void setRemoteRemoveRequestedHandler(std::function<void(const QString &, bool)> handler);
    void setRemoteRenameRequestedHandler(std::function<void(const QString &, const QString &)> handler);
    void setRemotePermissionsRequestedHandler(std::function<void(const QString &, int, bool)> handler);
    void setLocalUploadRequestedHandler(std::function<void(const QString &)> handler);
    void setRemoteDownloadRequestedHandler(std::function<void(const QString &, bool)> handler);
    void setRemoteEditRequestedHandler(std::function<void(const QString &)> handler);
    void setRemoteEditActiveQuery(std::function<bool(const QString &)> query);
    void setRemoteEditCloseRequestedHandler(std::function<void(const QString &)> handler);
    /**
     * @brief 设置本地文件拖放到远程面板时使用的回调。
     * @param handler 接收待上传本地文件路径的回调。
     */
    void setLocalFilesDroppedOnRemoteHandler(std::function<void(const QStringList &)> handler);
    /**
     * @brief 设置远程文件拖放到本地面板时使用的回调。
     * @param handler 接收带文件类型的远程项目列表。
     */
    void setRemoteFilesDroppedOnLocalHandler(std::function<void(const QList<RemoteTransferItem> &)> handler);
    /**
     * @brief 设置远程项目拖放到远程目录时使用的回调。
     * @param handler 接收远程源路径列表和目标远程目录的回调。
     */
    void setRemoteFilesDroppedOnRemoteHandler(std::function<void(const QStringList &, const QString &)> handler);
    QString currentPath() const;
    void setRemoteSummary(const QString &curlVersion, bool hasFtp, bool hasSftp);
    void setRemoteKnownDirectories(const QStringList &directories);
    void setRemoteItems(const QString &path, const std::vector<FileItem> &items, const QString &status, bool addToHistory = true);
    /**
     * @brief 显示远程会话连接过程中的过渡状态。
     * @param status 面向用户的连接进度文本。
     */
    void setRemoteConnecting(const QString &status);
    /**
     * @brief 显示远程目录正在后台加载的状态。
     * @param path 正在请求的远程目录。
     */
    void setRemoteLoading(const QString &path);
    void setRemoteDisconnected(const QString &status);
    void setRemoteError(const QString &status);

    /**
     * @brief 在自动化测试中将本地面板切换到指定目录。
     * @param path 作为当前面板路径使用的本地目录路径。
     */
    void setLocalPathForTesting(const QString &path);

protected:
    /**
     * @brief 处理文件传输手势对应的表格拖拽事件。
     * @param watched 接收事件的对象。
     * @param event 待检查的事件。
     * @return 事件已被消费时返回 true。
     */
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void setupUi();
    void connectSignals();
    void initialize();
    void navigateTo(const QString &path, bool addToHistory = true);
    void navigateUp();
    void navigateBack();
    void navigateForward();
    void requestRemotePath(const QString &path, bool addToHistory);
    void showLocalContextMenu(const QPoint &position);
    void showRemoteContextMenu(const QPoint &position);
    void showUnifiedContextMenu(const QPoint &position);
    /**
     * @brief 对远程表格应用类似资源管理器的排序规则。
     * @param column 用户点击的表头列。
     */
    void sortRemoteItemsByColumn(int column);
    /**
     * @brief 为远程项目或当前目录显示简易属性对话框。
     * @param path 要显示属性的远程路径。
     */
    void showRemoteProperties(const QString &path) const;
    void showRemotePermissionsDialog(const QString &path, bool isDirectory);
    void createLocalDirectory();
    void createLocalFile();
    void removeLocalPath(const QString &path);
    void finishLocalRemove(const QString &path, bool removed);
    void renameLocalPath(const QString &path);
    void renameSelectedEntry();
    void showLocalProperties(const QString &path) const;
    void startDragFromSelection();
    bool canAcceptTransferDrop(const QMimeData *mimeData) const;
    void handleTransferDrop(const QMimeData *mimeData, const QPoint &position);
    QString remoteDropTargetDirectory(const QPoint &position) const;
    QList<RemoteTransferItem> selectedFileTransferItems() const;
    QString remoteChildPath(const QString &name) const;
    QString remoteSiblingPath(const QString &sourcePath, const QString &newName) const;
    void updateNavigationButtons();
    void updateRemoteNavigationButtons();
    void populateLocalDirectory(const QString &path);
    void populateRemotePlaceholder();
    void populateRemoteItems(const QString &path, const std::vector<FileItem> &items, const QString &status);
    void updateLocalTreeSelection(const QString &path);
    void updateRemoteTree(const QString &path, const std::vector<FileItem> &items);
    QTableWidgetItem *createItem(const QString &text, const QIcon &icon = QIcon()) const;
    QString selectedEntryPath() const;

private:
    Mode m_mode;
    QString m_currentPath;
    QStringList m_history;
    int m_historyIndex = -1;
    int m_remoteSortColumn = 0;
    Qt::SortOrder m_remoteSortOrder = Qt::AscendingOrder;
    QFileIconProvider m_iconProvider;
    std::vector<FileItem> m_remoteItems;
    QStringList m_remoteKnownDirectories;
    QSet<QString> m_pendingLocalDeletes;
    std::function<void(bool)> m_fileTreeVisibilityRequested;
    std::function<void(const QString &, bool)> m_remotePathRequested;
    std::function<void()> m_remoteRefreshRequested;
    std::function<void(const QString &)> m_remoteCreateDirectoryRequested;
    std::function<void(const QString &)> m_remoteCreateFileRequested;
    std::function<void(const QString &, bool)> m_remoteRemoveRequested;
    std::function<void(const QString &, const QString &)> m_remoteRenameRequested;
    std::function<void(const QString &, int, bool)> m_remotePermissionsRequested;
    std::function<void(const QString &)> m_localUploadRequested;
    std::function<void(const QString &, bool)> m_remoteDownloadRequested;
    std::function<void(const QString &)> m_remoteEditRequested;
    std::function<bool(const QString &)> m_remoteEditActiveQuery;
    std::function<void(const QString &)> m_remoteEditCloseRequested;
    std::function<void(const QStringList &)> m_localFilesDroppedOnRemote;
    std::function<void(const QList<RemoteTransferItem> &)> m_remoteFilesDroppedOnLocal;
    std::function<void(const QStringList &, const QString &)> m_remoteFilesDroppedOnRemote;

    QPushButton *m_backButton = nullptr;
    QPushButton *m_forwardButton = nullptr;
    QPushButton *m_upButton = nullptr;
    QPushButton *m_refreshButton = nullptr;
    QLineEdit *m_pathEdit = nullptr;
    QLabel *m_stateLabel = nullptr;
    QSplitter *m_contentSplitter = nullptr;
    QTreeWidget *m_localTree = nullptr;
    QTreeWidget *m_remoteTree = nullptr;
    QTableWidget *m_table = nullptr;
    QPoint m_dragStartPosition;
};

#endif // DIRBRIDGE_UI_FILEPANEL_H
