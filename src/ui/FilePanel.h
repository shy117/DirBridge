#ifndef DIRBRIDGE_UI_FILEPANEL_H
#define DIRBRIDGE_UI_FILEPANEL_H

#include <QFileIconProvider>
#include <QPoint>
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
    void refresh();
    void setRemotePathRequestedHandler(std::function<void(const QString &, bool)> handler);
    void setRemoteRefreshRequestedHandler(std::function<void()> handler);
    void setRemoteCreateDirectoryRequestedHandler(std::function<void(const QString &)> handler);
    /**
     * @brief Sets the callback used when the remote context menu requests a new empty file.
     * @param handler Callback receiving the absolute remote file path.
     */
    void setRemoteCreateFileRequestedHandler(std::function<void(const QString &)> handler);
    void setRemoteRemoveRequestedHandler(std::function<void(const QString &)> handler);
    void setRemoteRenameRequestedHandler(std::function<void(const QString &, const QString &)> handler);
    void setLocalUploadRequestedHandler(std::function<void(const QString &)> handler);
    void setRemoteDownloadRequestedHandler(std::function<void(const QString &)> handler);
    /**
     * @brief Sets the callback used when local files are dropped onto the remote panel.
     * @param handler Callback receiving local file paths to upload.
     */
    void setLocalFilesDroppedOnRemoteHandler(std::function<void(const QStringList &)> handler);
    /**
     * @brief Sets the callback used when remote files are dropped onto the local panel.
     * @param handler Callback receiving remote file paths to download.
     */
    void setRemoteFilesDroppedOnLocalHandler(std::function<void(const QStringList &)> handler);
    QString currentPath() const;
    void setRemoteSummary(const QString &curlVersion, bool hasFtp, bool hasSftp);
    void setRemoteItems(const QString &path, const std::vector<FileItem> &items, const QString &status, bool addToHistory = true);
    void setRemoteDisconnected(const QString &status);
    void setRemoteError(const QString &status);

    /**
     * @brief Navigates the local panel to a directory for automated tests.
     * @param path Local directory path to use as the current panel path.
     */
    void setLocalPathForTesting(const QString &path);

protected:
    /**
     * @brief Handles table drag-and-drop events for file transfer gestures.
     * @param watched Object receiving the event.
     * @param event Event to inspect.
     * @return true when the event was consumed.
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
     * @brief Applies resource-manager style sorting to the remote table.
     * @param column Header column selected by the user.
     */
    void sortRemoteItemsByColumn(int column);
    /**
     * @brief Shows a simple property dialog for a remote item or current directory.
     * @param path Remote path whose properties should be displayed.
     */
    void showRemoteProperties(const QString &path) const;
    void createLocalDirectory();
    void createLocalFile();
    void removeLocalPath(const QString &path);
    void renameLocalPath(const QString &path);
    void showLocalProperties(const QString &path) const;
    void startDragFromSelection();
    bool canAcceptTransferDrop(const QMimeData *mimeData) const;
    void handleTransferDrop(const QMimeData *mimeData);
    QStringList selectedFileTransferPaths() const;
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
    std::function<void(const QString &, bool)> m_remotePathRequested;
    std::function<void()> m_remoteRefreshRequested;
    std::function<void(const QString &)> m_remoteCreateDirectoryRequested;
    std::function<void(const QString &)> m_remoteCreateFileRequested;
    std::function<void(const QString &)> m_remoteRemoveRequested;
    std::function<void(const QString &, const QString &)> m_remoteRenameRequested;
    std::function<void(const QString &)> m_localUploadRequested;
    std::function<void(const QString &)> m_remoteDownloadRequested;
    std::function<void(const QStringList &)> m_localFilesDroppedOnRemote;
    std::function<void(const QStringList &)> m_remoteFilesDroppedOnLocal;

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
