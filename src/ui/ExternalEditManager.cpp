#include "ui/ExternalEditManager.h"

#include "core/RemoteFileSystem.h"
#include "ui/FileChangeMonitor.h"

#include <QDesktopServices>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <filesystem>
#include <utility>

namespace
{
constexpr std::int64_t maximumExternalEditFileSize = 5 * 1024 * 1024;
constexpr std::size_t maximumOpenExternalEditDocuments = 5;
constexpr int externalEditInactivityMilliseconds = 5 * 60 * 1000;

QString parentRemotePath(const QString &path)
{
    QString normalized = path;
    while (normalized.size() > 1 && normalized.endsWith('/'))
    {
        normalized.chop(1);
    }
    const int separator = normalized.lastIndexOf('/');
    return separator <= 0 ? QString("/") : normalized.left(separator);
}

RemoteFileRevision queryRevision(RemoteFileSystem &fileSystem, const std::string &remotePath)
{
    const QString path = QString::fromStdString(remotePath);
    const std::vector<FileItem> siblings = fileSystem.listDirectory(parentRemotePath(path).toStdString());
    const auto item = std::find_if(siblings.begin(), siblings.end(), [&remotePath](const FileItem &candidate) {
        return candidate.path == remotePath;
    });
    if (item == siblings.end())
    {
        return {};
    }

    RemoteFileRevision revision;
    revision.size = item->size;
    revision.modifiedTime = item->modifiedTime;
    revision.reliable = item->size >= 0 && !item->modifiedTime.empty();
    return revision;
}

bool revisionsConflict(const RemoteFileRevision &baseline, const RemoteFileRevision &current)
{
    return baseline.reliable
        && current.reliable
        && (baseline.size != current.size || baseline.modifiedTime != current.modifiedTime);
}
}

ExternalEditManager::ExternalEditManager(std::filesystem::path cacheRoot,
                                         Callbacks callbacks,
                                         QObject *parent)
    : QObject(parent)
    , m_cache(std::move(cacheRoot))
    , m_callbacks(std::move(callbacks))
{
}

ExternalEditManager::~ExternalEditManager() = default;

void ExternalEditManager::openRemoteFile(const QString &sessionId, const QString &remotePath)
{
    const QString normalizedSessionId = sessionId.trimmed();
    const QString normalizedRemotePath = remotePath.trimmed();
    if (normalizedSessionId.isEmpty() || normalizedRemotePath.isEmpty())
    {
        showWarning("无法编辑远程文件", "远程会话或文件路径无效。");
        return;
    }

    const FileCacheEntry cacheEntry = m_cache.createEntry(
        normalizedSessionId.toStdString(),
        normalizedRemotePath.toStdString());
    if (ManagedDocument *existing = findDocument(cacheEntry.documentId))
    {
        if (existing->document->state() != ExternalEditState::Downloading
            && std::filesystem::exists(existing->document->cacheEntry().workingFilePath))
        {
            QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdWString(existing->document->cacheEntry().workingFilePath.wstring())));
        }
        return;
    }

    while (m_documents.size() >= maximumOpenExternalEditDocuments && !m_documentOrder.empty())
    {
        if (!closeDocumentById(m_documentOrder.front()))
        {
            showWarning("无法编辑远程文件", "已达到同时编辑文件数量上限，请先完成同步并关闭一个编辑文件。");
            return;
        }
    }

    const FileCacheResult cacheResult = m_cache.prepareEntry(cacheEntry);
    if (!cacheResult.success)
    {
        showWarning("无法编辑远程文件", QString("无法创建本地编辑缓存：%1").arg(QString::fromStdString(cacheResult.message)));
        return;
    }

    ManagedDocument managedDocument;
    managedDocument.document = std::make_unique<ExternalEditDocument>(
        normalizedSessionId.toStdString(), normalizedRemotePath.toStdString(), cacheEntry);
    managedDocument.monitor = std::make_unique<FileChangeMonitor>();
    managedDocument.inactivityTimer = std::make_unique<QTimer>();
    managedDocument.inactivityTimer->setSingleShot(true);
    managedDocument.inactivityTimer->setInterval(externalEditInactivityMilliseconds);
    const std::string documentId = cacheEntry.documentId;
    connect(managedDocument.monitor.get(), &FileChangeMonitor::stableFileChanged, this, [this, documentId](const QString &) {
        handleStableFileChanged(documentId);
    });
    connect(managedDocument.monitor.get(), &FileChangeMonitor::fileUnavailable, this, [this, documentId](const QString &) {
        handleFileUnavailable(documentId);
    });
    connect(managedDocument.inactivityTimer.get(), &QTimer::timeout, this, [this, documentId]() {
        closeDocumentById(documentId);
    });

    auto [iterator, inserted] = m_documents.emplace(documentId, std::move(managedDocument));
    Q_UNUSED(inserted);
    m_documentOrder.push_back(documentId);
    notifyState(*iterator->second.document);
    enqueueDownload(documentId);
}

void ExternalEditManager::setExternalFileLauncher(std::function<bool(const QString &)> launcher)
{
    m_callbacks.openExternalFile = std::move(launcher);
}

bool ExternalEditManager::isDocumentOpen(const QString &sessionId, const QString &remotePath) const
{
    const std::string documentId = FileCache::makeDocumentId(
        sessionId.trimmed().toStdString(),
        remotePath.trimmed().toStdString());
    return m_documents.find(documentId) != m_documents.end();
}

void ExternalEditManager::closeDocument(const QString &sessionId, const QString &remotePath)
{
    const std::string documentId = FileCache::makeDocumentId(
        sessionId.trimmed().toStdString(),
        remotePath.trimmed().toStdString());
    closeDocumentById(documentId);
}

void ExternalEditManager::closeSessionDocuments(const QString &sessionId)
{
    for (auto iterator = m_documents.begin(); iterator != m_documents.end();)
    {
        if (QString::fromStdString(iterator->second.document->sessionId()) != sessionId)
        {
            ++iterator;
            continue;
        }

        const std::string documentId = iterator->first;
        ++iterator;
        closeDocumentById(documentId, true);
    }
}

bool ExternalEditManager::hasActiveTasks() const
{
    return m_operationActive || !m_operations.empty();
}

ExternalEditManager::ManagedDocument *ExternalEditManager::findDocument(const std::string &documentId)
{
    const auto iterator = m_documents.find(documentId);
    return iterator == m_documents.end() ? nullptr : &iterator->second;
}

void ExternalEditManager::enqueueDownload(const std::string &documentId)
{
    m_operations.push_back({OperationType::Download, documentId});
    processOperations();
}

void ExternalEditManager::enqueueUpload(const std::string &documentId)
{
    if (!m_queuedUploads.insert(documentId).second)
    {
        return;
    }
    m_operations.push_back({OperationType::Upload, documentId});
    processOperations();
}

void ExternalEditManager::processOperations()
{
    if (m_operationActive || m_operations.empty())
    {
        return;
    }

    const PendingOperation operation = m_operations.front();
    ManagedDocument *managedDocument = findDocument(operation.documentId);
    if (managedDocument == nullptr)
    {
        m_operations.pop_front();
        m_queuedUploads.erase(operation.documentId);
        processOperations();
        return;
    }

    const QString sessionId = QString::fromStdString(managedDocument->document->sessionId());
    if (m_callbacks.canStartRemoteOperation && !m_callbacks.canStartRemoteOperation(sessionId))
    {
        if (!m_operationRetryScheduled)
        {
            m_operationRetryScheduled = true;
            QTimer::singleShot(250, this, [this]() {
                m_operationRetryScheduled = false;
                processOperations();
            });
        }
        return;
    }

    m_operations.pop_front();
    if (operation.type == OperationType::Download)
    {
        startDownload(*managedDocument);
    }
    else
    {
        m_queuedUploads.erase(operation.documentId);
        startUpload(*managedDocument);
    }
}

void ExternalEditManager::startDownload(ManagedDocument &managedDocument)
{
    const ExternalEditDocument &document = *managedDocument.document;
    const QString sessionId = QString::fromStdString(document.sessionId());
    const std::shared_ptr<RemoteFileSystem> fileSystem = m_callbacks.resolveFileSystem
        ? m_callbacks.resolveFileSystem(sessionId)
        : nullptr;
    if (fileSystem == nullptr)
    {
        managedDocument.document->failDownload();
        notifyState(*managedDocument.document);
        showWarning("无法编辑远程文件", "远程会话不可用。");
        processOperations();
        return;
    }

    m_operationActive = true;
    m_activeOperationDocumentId = document.id();
    if (m_callbacks.beginBackgroundTask)
    {
        m_callbacks.beginBackgroundTask();
    }
    const std::string documentId = document.id();
    const std::string remotePath = document.remotePath();
    const std::filesystem::path temporaryPath = document.cacheEntry().downloadTemporaryPath;
    QPointer<ExternalEditManager> manager(this);
    QThread *thread = QThread::create([manager, fileSystem, documentId, remotePath, temporaryPath]() {
        DownloadResult result;
        try
        {
            result.revision = queryRevision(*fileSystem, remotePath);
            if (result.revision.size > maximumExternalEditFileSize)
            {
                result.error = "remote file exceeds the 5 MiB external editing limit";
            }
            else
            {
                const RemoteOperationResult operationResult = fileSystem->downloadFile(remotePath, temporaryPath.string());
                result.success = operationResult.success;
                result.error = operationResult.message;
            }
        }
        catch (const std::exception &error)
        {
            result.error = error.what();
        }

        if (manager != nullptr)
        {
            QMetaObject::invokeMethod(manager.data(), [manager, documentId, result]() {
                if (manager != nullptr)
                {
                    manager->finishDownload(documentId, result);
                }
            }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, this, [this]() {
        if (m_callbacks.finishBackgroundTask)
        {
            m_callbacks.finishBackgroundTask();
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void ExternalEditManager::finishDownload(const std::string &documentId, const DownloadResult &result)
{
    m_operationActive = false;
    m_activeOperationDocumentId.clear();
    ManagedDocument *managedDocument = findDocument(documentId);
    if (managedDocument == nullptr)
    {
        processOperations();
        return;
    }

    if (!result.success)
    {
        managedDocument->document->failDownload();
        notifyState(*managedDocument->document);
        showWarning("下载远程文件失败", QString::fromStdString(result.error));
        processOperations();
        return;
    }

    const FileCacheResult cacheResult = m_cache.commitDownloadedFile(managedDocument->document->cacheEntry());
    if (!cacheResult.success)
    {
        managedDocument->document->failDownload();
        notifyState(*managedDocument->document);
        showWarning("准备编辑缓存失败", QString::fromStdString(cacheResult.message));
        processOperations();
        return;
    }

    managedDocument->document->completeDownload(result.revision);
    markDocumentSynchronized(documentId);
    const QString filePath = QString::fromStdWString(managedDocument->document->cacheEntry().workingFilePath.wstring());
    const bool opened = m_callbacks.openExternalFile
        ? m_callbacks.openExternalFile(filePath)
        : QDesktopServices::openUrl(QUrl::fromLocalFile(filePath));
    if (!opened)
    {
        managedDocument->document->failOpen();
        notifyState(*managedDocument->document);
        showWarning("无法打开外部编辑器", QString("无法使用系统默认应用打开：%1").arg(filePath));
        processOperations();
        return;
    }

    managedDocument->monitor->startMonitoring(filePath);
    restartInactivityTimer(*managedDocument);
    notifyState(*managedDocument->document);
    processOperations();
}

void ExternalEditManager::startUpload(ManagedDocument &managedDocument)
{
    std::optional<std::uint64_t> version = managedDocument.document->beginUpload();
    if (!version.has_value())
    {
        processOperations();
        return;
    }
    notifyState(*managedDocument.document);

    const ExternalEditDocument &document = *managedDocument.document;
    const QString sessionId = QString::fromStdString(document.sessionId());
    const std::shared_ptr<RemoteFileSystem> fileSystem = m_callbacks.resolveFileSystem
        ? m_callbacks.resolveFileSystem(sessionId)
        : nullptr;
    if (fileSystem == nullptr)
    {
        managedDocument.document->failUpload(*version);
        notifyState(*managedDocument.document);
        showWarning("同步远程文件失败", "远程会话不可用，本地修改已保留。");
        processOperations();
        return;
    }

    m_operationActive = true;
    m_activeOperationDocumentId = document.id();
    if (m_callbacks.beginBackgroundTask)
    {
        m_callbacks.beginBackgroundTask();
    }
    const std::string documentId = document.id();
    const std::string remotePath = document.remotePath();
    const FileCacheEntry cacheEntry = document.cacheEntry();
    const RemoteFileRevision baseline = document.remoteRevision();
    const std::filesystem::path cacheRoot = m_cache.rootDirectory();
    const std::uint64_t uploadVersion = *version;
    QPointer<ExternalEditManager> manager(this);
    QThread *thread = QThread::create([manager, fileSystem, documentId, remotePath, cacheEntry, cacheRoot, baseline, uploadVersion]() {
        UploadResult result;
        FileCache cache(cacheRoot);
        std::filesystem::path snapshotPath;
        const FileCacheResult snapshotResult = cache.createUploadSnapshot(cacheEntry, uploadVersion, snapshotPath);
        if (!snapshotResult.success)
        {
            result.error = snapshotResult.message;
        }
        else
        {
            try
            {
                const RemoteFileRevision currentRevision = queryRevision(*fileSystem, remotePath);
                if (revisionsConflict(baseline, currentRevision))
                {
                    result.conflict = true;
                    result.revision = currentRevision;
                }
                else
                {
                    const RemoteOperationResult operationResult = fileSystem->uploadFile(snapshotPath.string(), remotePath);
                    result.success = operationResult.success;
                    result.error = operationResult.message;
                    if (result.success)
                    {
                        result.revision = queryRevision(*fileSystem, remotePath);
                    }
                }
            }
            catch (const std::exception &error)
            {
                result.error = error.what();
            }
        }

        if (manager != nullptr)
        {
            QMetaObject::invokeMethod(manager.data(), [manager, documentId, uploadVersion, result]() {
                if (manager != nullptr)
                {
                    manager->finishUpload(documentId, uploadVersion, result);
                }
            }, Qt::QueuedConnection);
        }
    });
    connect(thread, &QThread::finished, this, [this]() {
        if (m_callbacks.finishBackgroundTask)
        {
            m_callbacks.finishBackgroundTask();
        }
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

void ExternalEditManager::finishUpload(const std::string &documentId, std::uint64_t version, const UploadResult &result)
{
    m_operationActive = false;
    m_activeOperationDocumentId.clear();
    ManagedDocument *managedDocument = findDocument(documentId);
    if (managedDocument == nullptr)
    {
        processOperations();
        return;
    }

    if (result.conflict)
    {
        managedDocument->document->markConflict();
        notifyState(*managedDocument->document);
        showWarning("远程文件已变化", "检测到远程文件可能已被其他客户端修改，本地编辑副本已保留。");
    }
    else if (!result.success)
    {
        managedDocument->document->failUpload(version);
        notifyState(*managedDocument->document);
        showWarning("同步远程文件失败", QString("本地修改已保留：%1").arg(QString::fromStdString(result.error)));
    }
    else
    {
        managedDocument->document->completeUpload(version, result.revision);
        markDocumentSynchronized(documentId);
        restartInactivityTimer(*managedDocument);
        notifyState(*managedDocument->document);
        if (m_callbacks.remoteFileSynchronized)
        {
            m_callbacks.remoteFileSynchronized(
                QString::fromStdString(managedDocument->document->sessionId()),
                QString::fromStdString(managedDocument->document->remotePath()));
        }
        if (managedDocument->document->hasPendingUpload())
        {
            enqueueUpload(documentId);
        }
    }
    processOperations();
}

void ExternalEditManager::handleStableFileChanged(const std::string &documentId)
{
    ManagedDocument *managedDocument = findDocument(documentId);
    if (managedDocument == nullptr)
    {
        return;
    }
    managedDocument->document->markLocalFileChanged();
    notifyState(*managedDocument->document);
    enqueueUpload(documentId);
}

void ExternalEditManager::handleFileUnavailable(const std::string &documentId)
{
    ManagedDocument *managedDocument = findDocument(documentId);
    if (managedDocument == nullptr)
    {
        return;
    }
    managedDocument->document->markFileUnavailable();
    notifyState(*managedDocument->document);
    showWarning("编辑缓存不可用", "外部编辑器使用的本地文件暂时不可用，DirBridge 未删除任何缓存副本。");
}

bool ExternalEditManager::closeDocumentById(const std::string &documentId, bool preserveUnsynchronizedCache)
{
    const auto iterator = m_documents.find(documentId);
    if (iterator == m_documents.end())
    {
        m_documentOrder.erase(std::remove(m_documentOrder.begin(), m_documentOrder.end(), documentId), m_documentOrder.end());
        return true;
    }

    const bool operationPending = m_activeOperationDocumentId == documentId
        || std::any_of(m_operations.begin(), m_operations.end(), [&documentId](const PendingOperation &operation) {
            return operation.documentId == documentId;
        });
    const bool hasUnsynchronizedChanges = iterator->second.document->localVersion()
        > iterator->second.document->synchronizedVersion();
    if (!preserveUnsynchronizedCache && (operationPending || hasUnsynchronizedChanges))
    {
        showWarning(
            "暂时无法关闭编辑",
            operationPending ? "该文件仍有下载或同步任务，请等待任务完成后再关闭。"
                             : "该文件仍有未同步的本地修改，请先等待同步成功后再关闭。");
        return false;
    }

    iterator->second.inactivityTimer->stop();
    iterator->second.monitor->stopMonitoring();

    if (!hasUnsynchronizedChanges && !operationPending)
    {
        const FileCacheResult cacheResult = m_cache.removeEntry(iterator->second.document->cacheEntry());
        if (!cacheResult.success)
        {
            if (!preserveUnsynchronizedCache)
            {
                const QString filePath = QString::fromStdWString(iterator->second.document->cacheEntry().workingFilePath.wstring());
                if (std::filesystem::exists(iterator->second.document->cacheEntry().workingFilePath))
                {
                    iterator->second.monitor->startMonitoring(filePath);
                }
                restartInactivityTimer(iterator->second);
                showWarning("关闭编辑失败", QString("无法清理本地编辑缓存：%1").arg(QString::fromStdString(cacheResult.message)));
                return false;
            }
        }
    }

    iterator->second.document->close();
    notifyState(*iterator->second.document);
    m_queuedUploads.erase(documentId);
    m_documentOrder.erase(std::remove(m_documentOrder.begin(), m_documentOrder.end(), documentId), m_documentOrder.end());
    m_documents.erase(iterator);
    return true;
}

void ExternalEditManager::markDocumentSynchronized(const std::string &documentId)
{
    m_documentOrder.erase(std::remove(m_documentOrder.begin(), m_documentOrder.end(), documentId), m_documentOrder.end());
    m_documentOrder.push_back(documentId);
}

void ExternalEditManager::restartInactivityTimer(ManagedDocument &managedDocument)
{
    if (managedDocument.inactivityTimer != nullptr)
    {
        managedDocument.inactivityTimer->start();
    }
}

void ExternalEditManager::notifyState(const ExternalEditDocument &document)
{
    if (m_callbacks.stateChanged)
    {
        m_callbacks.stateChanged(
            QString::fromStdString(document.sessionId()),
            QString::fromStdString(document.remotePath()),
            document.state());
    }
}

void ExternalEditManager::showWarning(const QString &title, const QString &message) const
{
    if (m_callbacks.showWarning)
    {
        m_callbacks.showWarning(title, message);
    }
}
