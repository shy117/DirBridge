#ifndef DIRBRIDGE_UI_EXTERNALEDITMANAGER_H
#define DIRBRIDGE_UI_EXTERNALEDITMANAGER_H

#include <QObject>
#include <QString>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>

#include "core/ExternalEditDocument.h"
#include "core/FileCache.h"

class FileChangeMonitor;
class RemoteFileSystem;
class QTimer;

/**
 * @brief 协调远程文件外部编辑的下载、监听和自动上传。
 */
class ExternalEditManager : public QObject
{
    Q_OBJECT

public:
    struct Callbacks
    {
        std::function<std::shared_ptr<RemoteFileSystem>(const QString &)> resolveFileSystem;
        std::function<bool(const QString &)> canStartRemoteOperation;
        std::function<void()> beginBackgroundTask;
        std::function<void()> finishBackgroundTask;
        std::function<void(const QString &, const QString &)> showWarning;
        std::function<void(const QString &, const QString &, ExternalEditState)> stateChanged;
        std::function<void(const QString &, const QString &)> remoteFileSynchronized;
        std::function<bool(const QString &)> openExternalFile;
    };

    ExternalEditManager(std::filesystem::path cacheRoot,
                        Callbacks callbacks,
                        QObject *parent = nullptr);
    ~ExternalEditManager() override;

    void openRemoteFile(const QString &sessionId, const QString &remotePath);
    void setExternalFileLauncher(std::function<bool(const QString &)> launcher);
    bool isDocumentOpen(const QString &sessionId, const QString &remotePath) const;
    void closeDocument(const QString &sessionId, const QString &remotePath);
    void closeSessionDocuments(const QString &sessionId);
    bool hasActiveTasks() const;

private:
    enum class OperationType
    {
        Download,
        Upload
    };

    struct PendingOperation
    {
        OperationType type = OperationType::Download;
        std::string documentId;
    };

    struct ManagedDocument
    {
        std::unique_ptr<ExternalEditDocument> document;
        std::unique_ptr<FileChangeMonitor> monitor;
        std::unique_ptr<QTimer> inactivityTimer;
    };

    struct DownloadResult
    {
        bool success = false;
        std::string error;
        RemoteFileRevision revision;
    };

    struct UploadResult
    {
        bool success = false;
        bool conflict = false;
        std::string error;
        RemoteFileRevision revision;
    };

    ManagedDocument *findDocument(const std::string &documentId);
    void enqueueDownload(const std::string &documentId);
    void enqueueUpload(const std::string &documentId);
    void processOperations();
    void startDownload(ManagedDocument &managedDocument);
    void finishDownload(const std::string &documentId, const DownloadResult &result);
    void startUpload(ManagedDocument &managedDocument);
    void finishUpload(const std::string &documentId, std::uint64_t version, const UploadResult &result);
    void handleStableFileChanged(const std::string &documentId);
    void handleFileUnavailable(const std::string &documentId);
    void closeDocumentById(const std::string &documentId);
    void markDocumentSynchronized(const std::string &documentId);
    void restartInactivityTimer(ManagedDocument &managedDocument);
    void notifyState(const ExternalEditDocument &document);
    void showWarning(const QString &title, const QString &message) const;

    FileCache m_cache;
    Callbacks m_callbacks;
    std::map<std::string, ManagedDocument> m_documents;
    std::deque<std::string> m_documentOrder;
    std::deque<PendingOperation> m_operations;
    std::set<std::string> m_queuedUploads;
    bool m_operationActive = false;
    bool m_operationRetryScheduled = false;
};

#endif // DIRBRIDGE_UI_EXTERNALEDITMANAGER_H
