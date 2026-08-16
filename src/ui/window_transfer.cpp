#include "ui/MainWindow.h"

#include "core/FileReplacement.h"
#include "core/TransferManager.h"
#include "ui/FilePanel.h"
#include "ui/TransferConflictDialog.h"
#include "ui/panel_shared.h"
#include "ui/window_shared.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <map>
#include <optional>
#include <stdexcept>

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QThread>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItem>

using namespace window_shared;

namespace
{
QString conflictRenameCandidate(const QString &originalName, bool isDirectory, int index)
{
    const QFileInfo info(originalName);
    const QString suffix = isDirectory ? QString() : info.completeSuffix();
    QString baseName = isDirectory || suffix.isEmpty()
        ? originalName
        : originalName.left(originalName.size() - suffix.size() - 1);
    if (baseName.isEmpty())
    {
        baseName = originalName;
    }

    const QString marker = QString(" (%1)").arg(index);
    return suffix.isEmpty()
        ? baseName + marker
        : baseName + marker + "." + suffix;
}

QString availableConflictName(
    const QString &originalName,
    bool isDirectory,
    const std::function<bool(const QString &)> &targetNameExists)
{
    for (int index = 1; index < 10000; ++index)
    {
        const QString candidate = conflictRenameCandidate(originalName, isDirectory, index);
        if (!targetNameExists(candidate))
        {
            return candidate;
        }
    }
    return {};
}

TransferConflictDialog::ItemDetails localConflictDetails(const QFileInfo &info)
{
    TransferConflictDialog::ItemDetails details;
    details.name = info.fileName();
    details.path = info.absoluteFilePath();
    details.kind = info.isDir()
        ? TransferConflictDialog::ItemKind::Directory
        : TransferConflictDialog::ItemKind::File;
    details.size = info.isFile() ? info.size() : -1;
    details.modifiedTime = info.lastModified();
    return details;
}

TransferConflictDialog::ItemDetails remoteConflictDetails(const FileItem &item, const QString &fallbackPath)
{
    TransferConflictDialog::ItemDetails details;
    details.name = item.name.empty() ? remoteBaseName(fallbackPath) : QString::fromStdString(item.name);
    details.path = item.path.empty() ? fallbackPath : QString::fromStdString(item.path);
    details.kind = item.type == FileItemType::Directory
        ? TransferConflictDialog::ItemKind::Directory
        : TransferConflictDialog::ItemKind::File;
    details.size = item.type == FileItemType::File ? item.size : -1;
    details.modifiedTime = panel_shared::parseRemoteModifiedTime(QString::fromStdString(item.modifiedTime));
    return details;
}

}

/**
 * @brief 将传输任务加入队列，并刷新传输表格。
 * @param job 待加入队列的传输任务。
 */
void MainWindow::enqueueTransferJob(const TransferJob &job)
{
    if (m_closePending)
    {
        return;
    }

    m_transferQueue.enqueue(job);
    refreshTransferTable();
}

/**
 * @brief 创建目录传输的父任务行，用于汇总子文件任务进度。
 * @param direction 传输方向。
 * @param name 目录显示名。
 * @param localPath 本地目录根路径。
 * @param remotePath 远程目录根路径。
 * @param session 当前远程会话。
 * @return 生成的父任务 id。
 */
QString MainWindow::enqueueDirectoryTransferParent(TransferDirection direction, const QString &name, const QString &localPath, const QString &remotePath, const RemoteSession &session)
{
    TransferJob parent;
    parent.id = makeTransferJobId(direction == TransferDirection::Upload ? "upload-dir" : "download-dir");
    parent.name = name.toStdString();
    parent.kind = TransferJobKind::Directory;
    parent.direction = direction;
    parent.status = TransferStatus::Preparing;
    parent.localPath = localPath.toStdString();
    parent.remotePath = remotePath.toStdString();
    parent.sessionId = session.id.toStdString();
    parent.sessionName = session.displayName.toStdString();
    parent.totalBytes = 0;
    parent.transferredBytes = 0;
    parent.startedAtMs = currentEpochMillis();
    parent.preparationFinished = false;
    enqueueTransferJob(parent);
    QTimer::singleShot(1000, this, [this, parentJobId = QString::fromStdString(parent.id)]() {
        refreshPreparingTransfer(parentJobId);
    });
    return QString::fromStdString(parent.id);
}

void MainWindow::refreshPreparingTransfer(const QString &parentJobId)
{
    const TransferJob *parent = m_transferQueue.find(parentJobId.toStdString());
    if (parent == nullptr || parent->preparationFinished
        || parent->status == TransferStatus::Canceled || parent->status == TransferStatus::Canceling)
    {
        return;
    }
    refreshTransferTable();
    QTimer::singleShot(1000, this, [this, parentJobId]() {
        refreshPreparingTransfer(parentJobId);
    });
}

/**
 * @brief 根据目录传输子任务状态刷新父任务聚合进度。
 */
void MainWindow::updateDirectoryTransferParents()
{
    for (const TransferJob &snapshot : m_transferQueue.jobs())
    {
        if (snapshot.kind != TransferJobKind::Directory)
        {
            continue;
        }

        TransferJob *parent = m_transferQueue.find(snapshot.id);
        if (parent == nullptr)
        {
            continue;
        }

        int totalChildren = 0;
        int finishedChildren = 0;
        std::int64_t totalBytes = 0;
        std::int64_t transferredBytes = 0;
        std::int64_t startedAtMs = 0;
        std::int64_t finishedAtMs = 0;
        double currentBytesPerSecond = 0.0;
        bool hasFailed = false;
        bool hasCanceled = false;
        bool hasRunning = false;
        bool hasPending = false;
        QStringList messages;

        for (const TransferJob &child : m_transferQueue.jobs())
        {
            if (child.parentId != parent->id)
            {
                continue;
            }

            ++totalChildren;
            if (child.totalBytes > 0)
            {
                totalBytes += child.totalBytes;
                transferredBytes += std::max<std::int64_t>(0, std::min(child.transferredBytes, child.totalBytes));
            }
            if (child.status == TransferStatus::Completed || child.status == TransferStatus::Failed || child.status == TransferStatus::Canceled)
            {
                ++finishedChildren;
            }
            if (child.startedAtMs > 0 && (startedAtMs == 0 || child.startedAtMs < startedAtMs))
            {
                startedAtMs = child.startedAtMs;
            }
            if (child.finishedAtMs > finishedAtMs)
            {
                finishedAtMs = child.finishedAtMs;
            }
            if (child.status == TransferStatus::Running)
            {
                currentBytesPerSecond += child.currentBytesPerSecond;
            }
            hasFailed = hasFailed || child.status == TransferStatus::Failed;
            hasCanceled = hasCanceled || child.status == TransferStatus::Canceled || child.status == TransferStatus::Canceling;
            hasRunning = hasRunning || child.status == TransferStatus::Running;
            hasPending = hasPending || child.status == TransferStatus::Pending;
            const QString childMessage = transferMessageText(child);
            if (!childMessage.isEmpty() && child.status != TransferStatus::Completed)
            {
                messages << childMessage;
            }
        }

        parent->totalChildren = totalChildren;
        parent->finishedChildren = finishedChildren;
        parent->totalBytes = totalBytes > 0 ? totalBytes : totalChildren;
        parent->transferredBytes = totalBytes > 0 ? transferredBytes : finishedChildren;
        if (parent->status == TransferStatus::Canceled || parent->status == TransferStatus::Canceling)
        {
            parent->currentBytesPerSecond = 0.0;
            parent->finishedAtMs = parent->finishedAtMs > 0 ? parent->finishedAtMs : currentEpochMillis();
            continue;
        }
        if (parent->preparationFailed)
        {
            parent->status = TransferStatus::Failed;
            parent->currentBytesPerSecond = 0.0;
            parent->finishedAtMs = parent->finishedAtMs > 0 ? parent->finishedAtMs : currentEpochMillis();
            continue;
        }
        if (startedAtMs > 0 && parent->startedAtMs <= 0)
        {
            parent->startedAtMs = startedAtMs;
        }
        parent->currentBytesPerSecond = currentBytesPerSecond;
        if (!parent->preparationFinished)
        {
            parent->status = TransferStatus::Preparing;
            parent->errorMessage = QString("已发现 %1 个文件，正在准备目录").arg(totalChildren).toStdString();
            continue;
        }
        if (totalChildren == 0 && parent->status == TransferStatus::Preparing)
        {
            parent->totalBytes = 0;
            parent->transferredBytes = 0;
        }
        else if (totalChildren == 0)
        {
            parent->status = TransferStatus::Completed;
        }
        else if (hasFailed)
        {
            parent->status = TransferStatus::Failed;
        }
        else if (hasCanceled)
        {
            parent->status = TransferStatus::Canceled;
        }
        else if (finishedChildren == totalChildren)
        {
            parent->status = TransferStatus::Completed;
        }
        else if (hasRunning || finishedChildren > 0)
        {
            parent->status = TransferStatus::Running;
        }
        else if (hasPending)
        {
            parent->status = TransferStatus::Pending;
        }
        if (parent->status == TransferStatus::Completed
            || parent->status == TransferStatus::Failed
            || parent->status == TransferStatus::Canceled)
        {
            parent->finishedAtMs = parent->finishedAtMs > 0 ? parent->finishedAtMs : std::max(finishedAtMs, currentEpochMillis());
            parent->currentBytesPerSecond = 0.0;
        }
        parent->errorMessage = messages.isEmpty()
            ? QString("%1 / %2 个文件").arg(finishedChildren).arg(totalChildren).toStdString()
            : messages.join("; ").toStdString();
    }
}

/**
 * @brief 调度下一个待执行传输任务，并在后台线程中执行。
 */
void MainWindow::processTransferQueue()
{
    if (m_closePending)
    {
        refreshTransferTable();
        return;
    }

    if (m_transferWorkerRunning)
    {
        return;
    }

    TransferJob *job = m_transferQueue.nextPending();
    if (job == nullptr)
    {
        refreshTransferTable();
        return;
    }

    RemoteSession *session = remoteSessionById(job->sessionId);
    if (session == nullptr || !session->connected || session->fileSystem == nullptr)
    {
        job->status = TransferStatus::Failed;
        job->errorMessage = "remote session is not available";
        refreshTransferTable();
        processTransferQueue();
        return;
    }

    job->status = TransferStatus::Running;
    job->errorMessage.clear();
    const std::int64_t transferStartedAtMs = currentEpochMillis();
    if (job->startedAtMs <= 0)
    {
        job->startedAtMs = transferStartedAtMs;
    }
    job->finishedAtMs = 0;
    job->lastProgressAtMs = transferStartedAtMs;
    job->lastProgressBytes = std::max<std::int64_t>(0, job->transferredBytes);
    job->currentBytesPerSecond = 0.0;
    const TransferJob jobSnapshot = *job;
    auto cancelFlag = std::make_shared<std::atomic_bool>(false);
    m_transferCancelFlags[jobSnapshot.id] = cancelFlag;
    m_runningTransferJobId = QString::fromStdString(jobSnapshot.id);
    m_transferWorkerRunning = true;
    refreshTransferTable();

    const std::shared_ptr<RemoteFileSystem> fileSystem = session->fileSystem;
    QPointer<MainWindow> window(this);
    QThread *thread = QThread::create([window, fileSystem, jobSnapshot, cancelFlag]() {
        auto lastProgressAt = std::chrono::steady_clock::now() - std::chrono::milliseconds(250);
        const QString jobId = QString::fromStdString(jobSnapshot.id);
        auto progress = [window, jobId, cancelFlag, &lastProgressAt](std::int64_t transferredBytes, std::int64_t totalBytes) {
            if (cancelFlag->load())
            {
                return false;
            }

            const auto now = std::chrono::steady_clock::now();
            const bool terminalProgress = totalBytes > 0 && transferredBytes >= totalBytes;
            if (terminalProgress || now - lastProgressAt >= std::chrono::milliseconds(100))
            {
                lastProgressAt = now;
                if (window != nullptr)
                {
                    QMetaObject::invokeMethod(window.data(), [window, jobId, transferredBytes, totalBytes]() {
                        if (window != nullptr)
                        {
                            window->handleTransferProgress(jobId, transferredBytes, totalBytes);
                        }
                    }, Qt::QueuedConnection);
                }
            }
            return !cancelFlag->load();
        };
        auto entryProgress = [window, parentJobId = QString::fromStdString(jobSnapshot.id)](
                                 const std::string &relativePath,
                                 std::int64_t transferredBytes,
                                 std::int64_t totalBytes,
                                 file_replacement::DirectoryEntryTransferState state) {
            if (window == nullptr)
            {
                return;
            }
            QMetaObject::invokeMethod(window.data(), [
                window,
                parentJobId,
                relativePath = QString::fromStdString(relativePath),
                transferredBytes,
                totalBytes,
                state]() {
                if (window != nullptr)
                {
                    window->handleDirectoryReplacementEntryProgress(
                        parentJobId,
                        relativePath,
                        transferredBytes,
                        totalBytes,
                        static_cast<int>(state));
                }
            }, Qt::QueuedConnection);
        };

        RemoteOperationResult result;
        if (fileSystem == nullptr)
        {
            result = {false, "remote session is not available"};
        }
        else if (jobSnapshot.direction == TransferDirection::Upload)
        {
            if (jobSnapshot.kind == TransferJobKind::DirectoryReplacement)
            {
                result = jobSnapshot.replaceExisting
                    ? file_replacement::uploadDirectoryReplacing(
                          *fileSystem,
                          jobSnapshot.localPath,
                          jobSnapshot.remotePath,
                          progress,
                          entryProgress)
                    : RemoteOperationResult{false, "directory replacement job is missing the replacement flag"};
            }
            else
            {
                result = jobSnapshot.replaceExisting
                    ? file_replacement::uploadFileReplacing(*fileSystem, jobSnapshot.localPath, jobSnapshot.remotePath, progress)
                    : fileSystem->uploadFile(jobSnapshot.localPath, jobSnapshot.remotePath, progress);
            }
        }
        else
        {
            if (jobSnapshot.kind == TransferJobKind::DirectoryReplacement)
            {
                result = jobSnapshot.replaceExisting
                    ? file_replacement::downloadDirectoryReplacing(
                          *fileSystem,
                          jobSnapshot.remotePath,
                          jobSnapshot.localPath,
                          progress,
                          entryProgress)
                    : RemoteOperationResult{false, "directory replacement job is missing the replacement flag"};
            }
            else
            {
                result = jobSnapshot.replaceExisting
                    ? file_replacement::downloadFileReplacing(*fileSystem, jobSnapshot.remotePath, jobSnapshot.localPath, progress)
                    : fileSystem->downloadFile(jobSnapshot.remotePath, jobSnapshot.localPath, progress);
            }
        }

        const bool canceled = cancelFlag->load();
        if (window != nullptr)
        {
            QMetaObject::invokeMethod(window.data(), [window, jobId, result, canceled]() {
                if (window != nullptr)
                {
                    window->handleTransferFinished(jobId, result, canceled);
                }
            }, Qt::QueuedConnection);
        }
    });
    m_transferThread = thread;
    beginBackgroundTask();
    connect(thread, &QThread::finished, this, [this]() {
        finishBackgroundTask();
    });
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    thread->start();
}

/**
 * @brief 接收后台传输进度并刷新对应任务行。
 * @param jobId 传输任务 id。
 * @param transferredBytes 已传输字节数。
 * @param totalBytes 总字节数。
 */
void MainWindow::handleTransferProgress(const QString &jobId, std::int64_t transferredBytes, std::int64_t totalBytes)
{
    TransferJob *job = m_transferQueue.find(jobId.toStdString());
    if (job == nullptr
        || job->status == TransferStatus::Completed
        || job->status == TransferStatus::Failed
        || job->status == TransferStatus::Canceled)
    {
        return;
    }

    if (totalBytes > 0)
    {
        job->totalBytes = totalBytes;
    }
    const std::int64_t now = currentEpochMillis();
    const std::int64_t previousAt = job->lastProgressAtMs;
    const std::int64_t previousBytes = job->lastProgressBytes;
    job->transferredBytes = std::max<std::int64_t>(0, transferredBytes);
    if (previousAt > 0 && now > previousAt && job->transferredBytes >= previousBytes)
    {
        const double seconds = static_cast<double>(now - previousAt) / 1000.0;
        if (seconds > 0.0)
        {
            job->currentBytesPerSecond = static_cast<double>(job->transferredBytes - previousBytes) / seconds;
        }
    }
    job->lastProgressAtMs = now;
    job->lastProgressBytes = job->transferredBytes;
    scheduleTransferTableRefresh();
}

void MainWindow::handleDirectoryReplacementEntryProgress(
    const QString &parentJobId,
    const QString &relativePath,
    std::int64_t transferredBytes,
    std::int64_t totalBytes,
    int state)
{
    TransferJob *parent = m_transferQueue.find(parentJobId.toStdString());
    if (parent == nullptr || parent->kind != TransferJobKind::DirectoryReplacement)
    {
        return;
    }

    const QString normalizedRelativePath = QDir::fromNativeSeparators(relativePath);
    const QString localPath = QDir(QString::fromStdString(parent->localPath)).filePath(normalizedRelativePath);
    const QString remotePath = joinRemotePath(QString::fromStdString(parent->remotePath), normalizedRelativePath);
    TransferJob *entryJob = nullptr;
    for (const TransferJob &snapshot : m_transferQueue.jobs())
    {
        if (snapshot.parentId == parent->id
            && snapshot.localPath == localPath.toStdString()
            && snapshot.remotePath == remotePath.toStdString())
        {
            entryJob = m_transferQueue.find(snapshot.id);
            break;
        }
    }

    if (entryJob == nullptr)
    {
        TransferJob entry;
        entry.id = makeTransferJobId(parent->direction == TransferDirection::Upload
            ? "upload-replacement-entry"
            : "download-replacement-entry");
        entry.name = QFileInfo(normalizedRelativePath).fileName().toStdString();
        entry.kind = TransferJobKind::DirectoryEntry;
        entry.parentId = parent->id;
        entry.direction = parent->direction;
        entry.status = TransferStatus::Pending;
        entry.localPath = localPath.toStdString();
        entry.remotePath = remotePath.toStdString();
        entry.sessionId = parent->sessionId;
        entry.sessionName = parent->sessionName;
        entry.totalBytes = totalBytes;
        entry.transferredBytes = 0;
        const TransferJob &queuedEntry = m_transferQueue.enqueue(std::move(entry));
        entryJob = m_transferQueue.find(queuedEntry.id);
    }
    if (entryJob == nullptr)
    {
        return;
    }

    const auto entryState = static_cast<file_replacement::DirectoryEntryTransferState>(state);
    const std::int64_t now = currentEpochMillis();
    const std::int64_t previousAt = entryJob->lastProgressAtMs;
    const std::int64_t previousBytes = entryJob->lastProgressBytes;
    entryJob->transferredBytes = std::max<std::int64_t>(0, transferredBytes);
    if (totalBytes >= 0)
    {
        entryJob->totalBytes = totalBytes;
    }

    switch (entryState)
    {
    case file_replacement::DirectoryEntryTransferState::Pending:
        entryJob->status = TransferStatus::Pending;
        break;
    case file_replacement::DirectoryEntryTransferState::Running:
        entryJob->status = TransferStatus::Running;
        entryJob->startedAtMs = entryJob->startedAtMs > 0 ? entryJob->startedAtMs : now;
        break;
    case file_replacement::DirectoryEntryTransferState::Completed:
        entryJob->status = TransferStatus::Completed;
        entryJob->startedAtMs = entryJob->startedAtMs > 0 ? entryJob->startedAtMs : now;
        entryJob->finishedAtMs = now;
        if (entryJob->totalBytes < 0)
        {
            entryJob->totalBytes = entryJob->transferredBytes;
        }
        entryJob->currentBytesPerSecond = 0.0;
        break;
    case file_replacement::DirectoryEntryTransferState::Failed:
        entryJob->status = TransferStatus::Failed;
        entryJob->finishedAtMs = now;
        entryJob->currentBytesPerSecond = 0.0;
        break;
    }

    if (entryJob->status == TransferStatus::Running
        && previousAt > 0 && now > previousAt && entryJob->transferredBytes >= previousBytes)
    {
        const double seconds = static_cast<double>(now - previousAt) / 1000.0;
        entryJob->currentBytesPerSecond = seconds > 0.0
            ? static_cast<double>(entryJob->transferredBytes - previousBytes) / seconds
            : 0.0;
    }
    entryJob->lastProgressAtMs = now;
    entryJob->lastProgressBytes = entryJob->transferredBytes;
    scheduleTransferTableRefresh();
}

void MainWindow::scheduleTransferTableRefresh()
{
    if (m_transferTableRefreshScheduled)
    {
        return;
    }
    m_transferTableRefreshScheduled = true;
    QTimer::singleShot(100, this, [this]() {
        m_transferTableRefreshScheduled = false;
        refreshTransferTable();
    });
}

/**
 * @brief 处理后台传输完成、失败或取消后的队列状态。
 * @param jobId 传输任务 id。
 * @param result 远程后端返回的执行结果。
 * @param canceled 是否由用户取消触发。
 */
void MainWindow::handleTransferFinished(const QString &jobId, const RemoteOperationResult &result, bool canceled)
{
    TransferJob *job = m_transferQueue.find(jobId.toStdString());
    bool refreshRemoteDirectory = false;
    if (job != nullptr)
    {
        const bool wasCanceling = job->status == TransferStatus::Canceling;
        if (canceled || wasCanceling)
        {
            job->status = TransferStatus::Canceled;
            job->errorMessage = "transfer canceled";
        }
        else if (result.success)
        {
            job->status = TransferStatus::Completed;
            if (job->totalBytes >= 0)
            {
                job->transferredBytes = job->totalBytes;
            }
            job->errorMessage = result.message;
        }
        else
        {
            job->status = TransferStatus::Failed;
            job->errorMessage = result.message;
        }
        job->finishedAtMs = currentEpochMillis();
        job->currentBytesPerSecond = 0.0;

        if (job->kind == TransferJobKind::DirectoryReplacement)
        {
            for (const TransferJob &snapshot : m_transferQueue.jobs())
            {
                if (snapshot.parentId != job->id
                    || snapshot.status == TransferStatus::Completed
                    || snapshot.status == TransferStatus::Failed
                    || snapshot.status == TransferStatus::Canceled)
                {
                    continue;
                }
                TransferJob *entry = m_transferQueue.find(snapshot.id);
                if (entry != nullptr)
                {
                    entry->status = job->status == TransferStatus::Canceled
                        ? TransferStatus::Canceled
                        : TransferStatus::Failed;
                    entry->finishedAtMs = job->finishedAtMs;
                    entry->currentBytesPerSecond = 0.0;
                }
            }
        }

        if (job->direction == TransferDirection::Upload)
        {
            if (job->parentId.empty())
            {
                refreshRemoteDirectory = job->status == TransferStatus::Completed;
            }
            else
            {
                const TransferJob *parent = m_transferQueue.find(job->parentId);
                const bool allChildrenFinished = parent != nullptr
                    && parent->preparationFinished
                    && std::all_of(m_transferQueue.jobs().begin(), m_transferQueue.jobs().end(), [parent](const TransferJob &candidate) {
                        return candidate.parentId != parent->id
                            || candidate.status == TransferStatus::Completed
                            || candidate.status == TransferStatus::Failed
                            || candidate.status == TransferStatus::Canceled;
                    });
                refreshRemoteDirectory = allChildrenFinished;
            }
            if (refreshRemoteDirectory)
            {
                RemoteSession *session = remoteSessionById(job->sessionId);
                if (session != nullptr && session->connected)
                {
                    loadRemotePath(*session, session->currentPath, false);
                }
            }
        }
        else if (m_localPanel != nullptr)
        {
            bool refreshLocalDirectory = job->parentId.empty()
                && job->status == TransferStatus::Completed;
            if (!job->parentId.empty())
            {
                const TransferJob *parent = m_transferQueue.find(job->parentId);
                refreshLocalDirectory = parent != nullptr
                    && parent->preparationFinished
                    && std::all_of(m_transferQueue.jobs().begin(), m_transferQueue.jobs().end(), [parent](const TransferJob &candidate) {
                        return candidate.parentId != parent->id
                            || candidate.status == TransferStatus::Completed
                            || candidate.status == TransferStatus::Failed
                            || candidate.status == TransferStatus::Canceled;
                    });
            }
            if (refreshLocalDirectory)
            {
                m_localPanel->refresh();
            }
        }
    }

    m_transferCancelFlags.erase(jobId.toStdString());
    m_transferWorkerRunning = false;
    m_runningTransferJobId.clear();
    m_transferThread = nullptr;
    refreshTransferTable();
    processTransferQueue();
}

/**
 * @brief 判断指定会话是否仍有目录加载、删除或传输任务在运行。
 * @param sessionId 远程会话 id。
 * @return 存在未完成任务时返回 true。
 */
bool MainWindow::hasRunningTransferForSession(const QString &sessionId) const
{
    for (const std::unique_ptr<RemoteSession> &session : m_remoteSessions)
    {
        if (session != nullptr && session->id == sessionId && session->activeRemoteOperationCount > 0)
        {
            return true;
        }
    }

    const QString deletePrefix = sessionId + "\n";
    for (const QString &deleteKey : m_pendingRemoteDeletes)
    {
        if (deleteKey.startsWith(deletePrefix))
        {
            return true;
        }
    }

    for (const TransferJob &job : m_transferQueue.jobs())
    {
        if (QString::fromStdString(job.sessionId) == sessionId
            && (job.status == TransferStatus::Preparing
                || job.status == TransferStatus::Running
                || job.status == TransferStatus::Canceling))
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief 重建传输表格显示，并保留目录任务展开状态。
 */
void MainWindow::refreshTransferTable()
{
    if (m_transferTable == nullptr)
    {
        return;
    }

    updateDirectoryTransferParents();
    const QString selectedId = selectedTransferJobId();
    QScrollBar *scrollBar = m_transferTable->verticalScrollBar();
    const int previousScrollValue = scrollBar == nullptr ? 0 : scrollBar->value();
    const bool followLatest = scrollBar == nullptr
        || m_transferTable->topLevelItemCount() == 0
        || scrollBar->value() >= scrollBar->maximum() - 2;
    const QSignalBlocker selectionBlocker(m_transferTable);
    std::map<std::string, bool> expandedStates;
    for (int index = 0; index < m_transferTable->topLevelItemCount(); ++index)
    {
        QTreeWidgetItem *item = m_transferTable->topLevelItem(index);
        if (item != nullptr)
        {
            expandedStates[item->data(0, Qt::UserRole).toString().toStdString()] = item->isExpanded();
        }
    }

    auto installProgressBar = [this](QTreeWidgetItem *item, const TransferJob &job) {
        auto *bar = new QProgressBar(m_transferTable);
        bar->setRange(0, 100);
        bar->setValue(progressPercent(job));
        bar->setTextVisible(true);
        bar->setFormat("%p%");
        bar->setMinimumWidth(120);
        bar->setMaximumHeight(18);
        m_transferTable->setItemWidget(item, 2, bar);
    };

    m_transferTable->clear();
    std::map<std::string, QTreeWidgetItem *> parentItems;
    std::map<std::string, QTreeWidgetItem *> allItems;
    for (const TransferJob &job : m_transferQueue.jobs())
    {
        if (!job.parentId.empty())
        {
            continue;
        }

        auto *item = new QTreeWidgetItem({
            QString::fromStdString(job.name),
            transferStatusText(job.status),
            QString("%1%").arg(progressPercent(job)),
            transferSizeProgressText(job),
            QString::fromStdString(job.localPath),
            job.direction == TransferDirection::Upload ? "->" : "<-",
            QString::fromStdString(job.remotePath),
            transferSpeedText(job.currentBytesPerSecond),
            transferRemainingText(job),
            transferElapsedText(job)
        });
        item->setData(0, Qt::UserRole, QString::fromStdString(job.id));
        m_transferTable->addTopLevelItem(item);
        allItems[job.id] = item;
        installProgressBar(item, job);
        if (job.kind == TransferJobKind::Directory
            || job.kind == TransferJobKind::DirectoryReplacement)
        {
            parentItems[job.id] = item;
            const auto expanded = expandedStates.find(job.id);
            item->setExpanded(expanded != expandedStates.end() && expanded->second);
        }
    }

    for (const TransferJob &job : m_transferQueue.jobs())
    {
        if (job.parentId.empty())
        {
            continue;
        }

        const auto parent = parentItems.find(job.parentId);
        if (parent == parentItems.end())
        {
            continue;
        }

        auto *item = new QTreeWidgetItem(parent->second, {
            QString::fromStdString(job.name),
            transferStatusText(job.status),
            QString("%1%").arg(progressPercent(job)),
            transferSizeProgressText(job),
            QString::fromStdString(job.localPath),
            job.direction == TransferDirection::Upload ? "->" : "<-",
            QString::fromStdString(job.remotePath),
            transferSpeedText(job.currentBytesPerSecond),
            transferRemainingText(job),
            transferElapsedText(job)
        });
        item->setData(0, Qt::UserRole, QString::fromStdString(job.id));
        allItems[job.id] = item;
        installProgressBar(item, job);
    }

    const auto selected = allItems.find(selectedId.toStdString());
    if (selected != allItems.end())
    {
        m_transferTable->setCurrentItem(selected->second);
        selected->second->setSelected(true);
    }
    if (followLatest)
    {
        m_transferTable->scrollToBottom();
    }
    else if (scrollBar != nullptr)
    {
        scrollBar->setValue(std::min(previousScrollValue, scrollBar->maximum()));
    }
    updateTransferActionButtons();
}

/**
 * @brief 按当前选择和队列状态刷新取消、重试、清理按钮。
 */
void MainWindow::updateTransferActionButtons()
{
    const QString selectedId = selectedTransferJobId();
    const TransferJob *selectedJob = selectedId.isEmpty() ? nullptr : m_transferQueue.find(selectedId.toStdString());

    const bool canCancel = selectedJob != nullptr
        && selectedJob->kind != TransferJobKind::DirectoryEntry
        && (selectedJob->status == TransferStatus::Preparing
            || selectedJob->status == TransferStatus::Pending
            || selectedJob->status == TransferStatus::Running);
    const bool canRetry = selectedJob != nullptr
        && selectedJob->kind != TransferJobKind::DirectoryEntry
        && (selectedJob->status == TransferStatus::Failed || selectedJob->status == TransferStatus::Canceled);
    const bool hasFinishedJobs = std::any_of(m_transferQueue.jobs().begin(), m_transferQueue.jobs().end(), [](const TransferJob &job) {
        return job.status == TransferStatus::Completed
            || job.status == TransferStatus::Failed
            || job.status == TransferStatus::Canceled;
    });

    if (m_cancelTransferButton != nullptr)
    {
        m_cancelTransferButton->setEnabled(canCancel);
    }
    if (m_retryTransferButton != nullptr)
    {
        m_retryTransferButton->setEnabled(canRetry);
    }
    if (m_clearFinishedTransfersButton != nullptr)
    {
        m_clearFinishedTransfersButton->setEnabled(hasFinishedJobs);
    }
}

/**
 * @brief 获取传输表格当前选中的任务 id。
 * @return 当前任务 id；未选择时返回空字符串。
 */
QString MainWindow::selectedTransferJobId() const
{
    if (m_transferTable == nullptr || m_transferTable->selectedItems().isEmpty())
    {
        return {};
    }

    QTreeWidgetItem *item = m_transferTable->currentItem();
    if (item == nullptr)
    {
        item = m_transferTable->selectedItems().first();
    }

    return item == nullptr ? QString() : item->data(0, Qt::UserRole).toString();
}

/**
 * @brief 取消当前选中的传输任务，目录任务会连同子任务一起取消。
 */
void MainWindow::cancelSelectedTransferJob()
{
    const QString id = selectedTransferJobId();
    if (id.isEmpty())
    {
        return;
    }

    const TransferJob *selectedJob = m_transferQueue.find(id.toStdString());
    if (selectedJob != nullptr && selectedJob->kind == TransferJobKind::Directory)
    {
        bool changed = m_transferQueue.cancel(selectedJob->id, "用户取消目录传输");
        for (const TransferJob &snapshot : m_transferQueue.jobs())
        {
            if (snapshot.parentId == selectedJob->id)
            {
                changed = m_transferQueue.cancel(snapshot.id, "用户取消目录传输") || changed;
                const auto cancelFlag = m_transferCancelFlags.find(snapshot.id);
                if (cancelFlag != m_transferCancelFlags.end() && cancelFlag->second != nullptr)
                {
                    cancelFlag->second->store(true);
                }
            }
        }
        if (changed)
        {
            appendLog("INFO", QString("目录传输任务已取消：%1").arg(id));
            refreshTransferTable();
        }
        return;
    }

    if (m_transferQueue.cancel(id.toStdString(), "用户取消传输"))
    {
        const auto cancelFlag = m_transferCancelFlags.find(id.toStdString());
        if (cancelFlag != m_transferCancelFlags.end() && cancelFlag->second != nullptr)
        {
            cancelFlag->second->store(true);
        }
        appendLog("INFO", QString("传输任务已取消：%1").arg(id));
        refreshTransferTable();
    }
}

/**
 * @brief 重试当前选中的失败或已取消传输任务。
 */
void MainWindow::retrySelectedTransferJob()
{
    const QString id = selectedTransferJobId();
    if (id.isEmpty())
    {
        return;
    }

    const TransferJob *selectedJob = m_transferQueue.find(id.toStdString());
    if (selectedJob != nullptr && selectedJob->kind == TransferJobKind::Directory)
    {
        int retried = 0;
        QStringList retrySourceIds;
        for (const TransferJob &snapshot : m_transferQueue.jobs())
        {
            if (snapshot.parentId != selectedJob->id
                || (snapshot.status != TransferStatus::Failed && snapshot.status != TransferStatus::Canceled))
            {
                continue;
            }
            retrySourceIds << QString::fromStdString(snapshot.id);
        }
        for (const QString &sourceId : retrySourceIds)
        {
            TransferJob *child = m_transferQueue.find(sourceId.toStdString());
            if (child != nullptr)
            {
                child->status = TransferStatus::Pending;
                child->transferredBytes = 0;
                child->startedAtMs = 0;
                child->finishedAtMs = 0;
                child->lastProgressAtMs = 0;
                child->lastProgressBytes = 0;
                child->currentBytesPerSecond = 0.0;
                child->errorMessage.clear();
                ++retried;
            }
        }
        if (retried > 0)
        {
            appendLog("INFO", QString("已重试 %1 个目录传输子任务：%2").arg(retried).arg(id));
            refreshTransferTable();
            processTransferQueue();
        }
        return;
    }

    const QString retryId = QString::fromStdString(makeTransferJobId("retry"));
    if (m_transferQueue.retry(id.toStdString(), retryId.toStdString()) == nullptr)
    {
        return;
    }

    appendLog("INFO", QString("传输任务已重试：%1 -> %2").arg(id, retryId));
    refreshTransferTable();
    processTransferQueue();
}

/**
 * @brief 清理已完成、失败和已取消的历史传输任务。
 */
void MainWindow::clearFinishedTransferJobs()
{
    const std::size_t removed = m_transferQueue.clearFinished();
    if (removed == 0)
    {
        return;
    }

    appendLog("INFO", QString("已清理 %1 个传输历史任务").arg(removed));
    refreshTransferTable();
}

/**
 * @brief 检查远程路径是否存在，并可返回对应条目。
 * @param session 目标远程会话。
 * @param remotePath 待检查的远程路径。
 * @param item 可选的远程条目输出。
 * @return 路径存在时返回 true。
 */
bool MainWindow::remotePathExists(RemoteSession &session, const QString &remotePath, FileItem *item)
{
    const QString name = remoteBaseName(remotePath);
    const QString parent = name.isEmpty()
        ? QString("/")
        : remotePath.left(remotePath.lastIndexOf('/')).isEmpty() ? QString("/") : remotePath.left(remotePath.lastIndexOf('/'));
    if (!name.isEmpty())
    {
        try
        {
            const std::vector<FileItem> siblings = session.fileSystem->listDirectory(parent.toStdString());
            for (const FileItem &candidate : siblings)
            {
                if (QString::fromStdString(candidate.name) == name)
                {
                    if (item != nullptr)
                    {
                        *item = candidate;
                    }
                    return true;
                }
            }
        }
        catch (const std::exception &)
        {
        }
    }

    try
    {
        session.fileSystem->listDirectory(remotePath.toStdString());
        if (item != nullptr)
        {
            item->name = name.toStdString();
            item->path = remotePath.toStdString();
            item->type = FileItemType::Directory;
        }
        return true;
    }
    catch (const std::exception &)
    {
    }

    return false;
}

/**
 * @brief 创建单文件上传任务，并启动传输队列。
 * @param session 目标远程会话。
 * @param localPath 本地文件路径。
 */
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
    job.status = TransferStatus::Preparing;
    job.localPath = localPath.toStdString();
    job.remotePath = joinRemotePath(session.currentPath, localInfo.fileName()).toStdString();
    job.sessionId = session.id.toStdString();
    job.sessionName = session.displayName.toStdString();
    job.totalBytes = localInfo.size();
    job.transferredBytes = 0;
    job.startedAtMs = currentEpochMillis();
    enqueueTransferJob(job);
    startSingleFileUploadPreparation(session, QString::fromStdString(job.id));
}

/**
 * @brief 在当前远程会话中上传单个本地文件。
 * @param localPath 本地文件路径。
 */
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

/**
 * @brief 异步检查单文件上传目标是否已存在。
 * @param session 目标远程会话。
 * @param jobId 已创建的上传任务 id。
 */
void MainWindow::startSingleFileUploadPreparation(RemoteSession &session, const QString &jobId)
{
    TransferJob *job = m_transferQueue.find(jobId.toStdString());
    if (job == nullptr || !session.connected || session.fileSystem == nullptr)
    {
        return;
    }

    const std::shared_ptr<RemoteFileSystem> fileSystem = session.fileSystem;
    const QString remotePath = QString::fromStdString(job->remotePath);
    QPointer<MainWindow> window(this);
    QThread *thread = QThread::create([window, fileSystem, jobId, remotePath]() {
        bool exists = false;
        FileItem existingItem;
        QString errorMessage;
        QString parent = remoteBaseName(remotePath).isEmpty()
            ? QString("/")
            : remotePath.left(remotePath.lastIndexOf('/')).isEmpty() ? QString("/") : remotePath.left(remotePath.lastIndexOf('/'));
        const QString name = remoteBaseName(remotePath);
        try
        {
            const std::vector<FileItem> siblings = fileSystem->listDirectory(parent.toStdString());
            for (const FileItem &candidate : siblings)
            {
                if (QString::fromStdString(candidate.name) == name)
                {
                    exists = true;
                    existingItem = candidate;
                    break;
                }
            }
        }
        catch (const std::exception &error)
        {
            errorMessage = QString::fromUtf8(error.what());
        }

        if (window != nullptr)
        {
            QMetaObject::invokeMethod(window.data(), [window, jobId, exists, existingItem, errorMessage]() {
                if (window != nullptr)
                {
                    window->finishSingleFileUploadPreparation(jobId, exists, existingItem, errorMessage);
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
 * @brief 根据单文件上传预检查结果决定是否排队执行。
 * @param jobId 上传任务 id。
 * @param exists 远程目标是否已存在。
 * @param existingItem 已存在的远程条目。
 * @param errorMessage 预检查失败时的错误信息。
 */
void MainWindow::finishSingleFileUploadPreparation(const QString &jobId, bool exists, const FileItem &existingItem, const QString &errorMessage)
{
    TransferJob *job = m_transferQueue.find(jobId.toStdString());
    if (job == nullptr || job->status == TransferStatus::Canceled || job->status == TransferStatus::Canceling)
    {
        refreshTransferTable();
        return;
    }
    if (!errorMessage.trimmed().isEmpty())
    {
        job->status = TransferStatus::Failed;
        job->errorMessage = errorMessage.toStdString();
        appendLog("ERROR", QString("上传目标检查失败：%1").arg(errorMessage));
        refreshTransferTable();
        showWarningMessage("上传失败", userFacingRemoteError(errorMessage));
        return;
    }

    appendLog(
        "INFO",
        QString("上传准备完成：%1（%2 ms）")
            .arg(QString::fromStdString(job->name))
            .arg(std::max<std::int64_t>(0, currentEpochMillis() - job->startedAtMs)));

    auto queuePreparedJob = [this, job]() {
        job->status = TransferStatus::Pending;
        job->errorMessage.clear();
        refreshTransferTable();
        processTransferQueue();
    };

    if (!exists)
    {
        queuePreparedJob();
        return;
    }

    RemoteSession *session = remoteSessionById(job->sessionId);
    if (session == nullptr || !session->connected || session->fileSystem == nullptr)
    {
        job->status = TransferStatus::Failed;
        job->errorMessage = "remote session is not available";
        refreshTransferTable();
        return;
    }

    const QFileInfo localInfo(QString::fromStdString(job->localPath));
    const QString remotePath = QString::fromStdString(job->remotePath);
    const int slashIndex = remotePath.lastIndexOf('/');
    const QString remoteParent = slashIndex <= 0 ? QString("/") : remotePath.left(slashIndex);
    const auto targetNameExists = [&](const QString &candidateName) {
        return remotePathExists(*session, joinRemotePath(remoteParent, candidateName));
    };

    TransferConflictDialog::Options options;
    options.direction = TransferConflictDialog::Direction::Upload;
    options.source = localConflictDetails(localInfo);
    options.target = remoteConflictDetails(existingItem, remotePath);
    options.allowOverwrite = existingItem.type == FileItemType::File;
    options.allowApplyToAll = false;
    options.suggestedName = availableConflictName(localInfo.fileName(), false, targetNameExists);

    while (true)
    {
        TransferConflictDialog::Decision decision;
        if (m_dialogsSuppressedForTesting)
        {
            decision.action = TransferConflictDialog::Action::Rename;
            decision.newName = options.suggestedName;
        }
        else
        {
            TransferConflictDialog dialog(options, this);
            dialog.exec();
            decision = dialog.decision();
        }

        if (decision.action == TransferConflictDialog::Action::Cancel)
        {
            m_transferQueue.cancel(job->id, "用户取消上传");
            refreshTransferTable();
            return;
        }
        if (decision.action == TransferConflictDialog::Action::Skip)
        {
            m_transferQueue.cancel(job->id, "已跳过：上传目标已存在");
            refreshTransferTable();
            return;
        }
        if (decision.action == TransferConflictDialog::Action::Overwrite)
        {
            job->replaceExisting = true;
            queuePreparedJob();
            return;
        }

        const QString newName = decision.newName.trimmed();
        if (!panel_shared::isValidRemoteName(newName))
        {
            panel_shared::showInvalidRemoteNameWarning(this);
            options.suggestedName = newName;
            continue;
        }
        if (targetNameExists(newName))
        {
            panel_shared::showFileOperationWarning(this, "上传目标重名", QString("目标项目仍然存在：%1").arg(newName));
            options.suggestedName = newName;
            continue;
        }
        job->replaceExisting = false;
        job->remotePath = joinRemotePath(remoteParent, newName).toStdString();
        queuePreparedJob();
        return;
    }
}

/**
 * @brief 上传本地文件或目录，目录会先创建父任务再异步收集子任务。
 * @param session 目标远程会话。
 * @param localPath 本地文件或目录路径。
 */
void MainWindow::uploadLocalPath(RemoteSession &session, const QString &localPath)
{
    const QFileInfo localInfo(localPath);
    if (localInfo.isFile())
    {
        uploadLocalFile(session, localPath);
        return;
    }

    if (!localInfo.isDir())
    {
        showWarningMessage("上传失败", QString("本地项目不可用：%1").arg(localPath));
        return;
    }

    QString remoteDirectoryPath = joinRemotePath(session.currentPath, localInfo.fileName());
    bool replaceDirectory = false;
    FileItem existingRemoteItem;
    if (remotePathExists(session, remoteDirectoryPath, &existingRemoteItem))
    {
        const auto targetNameExists = [&](const QString &candidateName) {
            return remotePathExists(session, joinRemotePath(session.currentPath, candidateName));
        };
        TransferConflictDialog::Options options;
        options.direction = TransferConflictDialog::Direction::Upload;
        options.source = localConflictDetails(localInfo);
        options.target = remoteConflictDetails(existingRemoteItem, remoteDirectoryPath);
        options.allowOverwrite = existingRemoteItem.type == FileItemType::Directory;
        options.allowApplyToAll = false;
        options.suggestedName = availableConflictName(localInfo.fileName(), true, targetNameExists);

        while (true)
        {
            TransferConflictDialog::Decision decision;
            if (m_dialogsSuppressedForTesting)
            {
                decision.action = TransferConflictDialog::Action::Rename;
                decision.newName = options.suggestedName;
            }
            else
            {
                TransferConflictDialog dialog(options, this);
                dialog.exec();
                decision = dialog.decision();
            }

            if (decision.action == TransferConflictDialog::Action::Cancel)
            {
                appendLog("INFO", QString("用户取消目录上传：%1").arg(localPath));
                return;
            }
            if (decision.action == TransferConflictDialog::Action::Skip)
            {
                appendLog("INFO", QString("已跳过目录上传，目标存在：%1").arg(remoteDirectoryPath));
                return;
            }
            if (decision.action == TransferConflictDialog::Action::Overwrite)
            {
                replaceDirectory = true;
                break;
            }

            const QString newName = decision.newName.trimmed();
            if (!panel_shared::isValidRemoteName(newName))
            {
                panel_shared::showInvalidRemoteNameWarning(this);
                options.suggestedName = newName;
                continue;
            }
            if (targetNameExists(newName))
            {
                panel_shared::showFileOperationWarning(this, "上传目标重名", QString("目标项目仍然存在：%1").arg(newName));
                options.suggestedName = newName;
                continue;
            }
            remoteDirectoryPath = joinRemotePath(session.currentPath, newName);
            break;
        }
    }
    if (replaceDirectory)
    {
        TransferJob job;
        job.id = makeTransferJobId("upload-dir-replace");
        job.name = localInfo.fileName().toStdString();
        job.kind = TransferJobKind::DirectoryReplacement;
        job.direction = TransferDirection::Upload;
        job.status = TransferStatus::Pending;
        job.localPath = localPath.toStdString();
        job.remotePath = remoteDirectoryPath.toStdString();
        job.sessionId = session.id.toStdString();
        job.sessionName = session.displayName.toStdString();
        job.totalBytes = 0;
        job.transferredBytes = 0;
        job.startedAtMs = currentEpochMillis();
        job.replaceExisting = true;
        enqueueTransferJob(job);
        processTransferQueue();
        return;
    }
    const QString parentJobId = enqueueDirectoryTransferParent(
        TransferDirection::Upload,
        localInfo.fileName(),
        localPath,
        remoteDirectoryPath,
        session);
    startLocalDirectoryUploadPreparation(session, localPath, remoteDirectoryPath, parentJobId);
}

/**
 * @brief 异步扫描本地目录，并生成目录上传子任务。
 * @param session 目标远程会话。
 * @param localDirectoryPath 本地目录根路径。
 * @param remoteDirectoryPath 远程目录根路径。
 * @param parentJobId 目录父任务 id。
 */
void MainWindow::startLocalDirectoryUploadPreparation(RemoteSession &session, const QString &localDirectoryPath, const QString &remoteDirectoryPath, const QString &parentJobId)
{
    if (!session.connected || session.fileSystem == nullptr)
    {
        handlePreparedDirectoryTransfer(parentJobId, {}, "请先连接远程会话。");
        return;
    }

    const std::shared_ptr<RemoteFileSystem> fileSystem = session.fileSystem;
    const QString sessionId = session.id;
    const QString sessionName = session.displayName;
    QPointer<MainWindow> window(this);

    QThread *thread = QThread::create([window, fileSystem, sessionId, sessionName, localDirectoryPath, remoteDirectoryPath, parentJobId]() {
        std::vector<TransferJob> pendingJobs;
        QString errorMessage;
        auto lastFlush = std::chrono::steady_clock::now();

        auto flushJobs = [&]() {
            if (pendingJobs.empty() || window == nullptr)
            {
                return;
            }
            std::vector<TransferJob> jobs;
            jobs.swap(pendingJobs);
            QMetaObject::invokeMethod(window.data(), [window, parentJobId, jobs = std::move(jobs)]() {
                if (window != nullptr)
                {
                    window->appendPreparedDirectoryTransferJobs(parentJobId, jobs);
                }
            }, Qt::QueuedConnection);
            lastFlush = std::chrono::steady_clock::now();
        };

        auto ensureDirectory = [fileSystem](const QString &path, QString *error) {
            const RemoteOperationResult result = fileSystem->createDirectory(path.toStdString());
            if (result.success)
            {
                return true;
            }
            try
            {
                fileSystem->listDirectory(path.toStdString());
                return true;
            }
            catch (const std::exception &exception)
            {
                if (error != nullptr)
                {
                    *error = QString("远程目录创建失败：%1；确认目录失败：%2")
                        .arg(QString::fromStdString(result.message), QString::fromUtf8(exception.what()));
                }
                return false;
            }
        };

        const QFileInfo rootInfo(localDirectoryPath);
        if (!rootInfo.isDir())
        {
            errorMessage = QString("本地目录不存在：%1").arg(localDirectoryPath);
        }
        else if (ensureDirectory(remoteDirectoryPath, &errorMessage))
        {
            QDirIterator iterator(
                localDirectoryPath,
                QDir::AllEntries | QDir::NoDotAndDotDot,
                QDirIterator::Subdirectories);
            const QDir rootDir(localDirectoryPath);
            while (iterator.hasNext())
            {
                const QString localPath = iterator.next();
                const QFileInfo info(localPath);
                const QString relativePath = rootDir.relativeFilePath(localPath);
                const QString remotePath = joinRemotePath(remoteDirectoryPath, relativePath);
                if (info.isDir())
                {
                    if (!ensureDirectory(remotePath, &errorMessage))
                    {
                        break;
                    }
                    continue;
                }
                if (!info.isFile())
                {
                    continue;
                }

                TransferJob job;
                job.id = makeTransferJobId("upload");
                job.name = info.fileName().toStdString();
                job.kind = TransferJobKind::File;
                job.parentId = parentJobId.toStdString();
                job.direction = TransferDirection::Upload;
                job.status = TransferStatus::Pending;
                job.localPath = localPath.toStdString();
                job.remotePath = remotePath.toStdString();
                job.sessionId = sessionId.toStdString();
                job.sessionName = sessionName.toStdString();
                job.totalBytes = info.size();
                job.transferredBytes = 0;
                pendingJobs.push_back(std::move(job));
                const auto now = std::chrono::steady_clock::now();
                if (pendingJobs.size() >= 16
                    || std::chrono::duration_cast<std::chrono::milliseconds>(now - lastFlush).count() >= 100)
                {
                    flushJobs();
                }
            }
        }

        flushJobs();
        if (window != nullptr)
        {
            QMetaObject::invokeMethod(window.data(), [window, parentJobId, errorMessage]() {
                if (window != nullptr)
                {
                    window->finishPreparedDirectoryTransfer(parentJobId, errorMessage);
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
 * @brief 创建单文件下载任务，并启动传输队列。
 * @param session 目标远程会话。
 * @param remotePath 远程文件路径。
 */
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
    QString localPath = QDir(m_localPanel->currentPath()).filePath(remoteInfo.fileName());
    bool replaceExisting = false;
    if (QFileInfo::exists(localPath))
    {
        const QFileInfo targetInfo(localPath);
        const auto targetNameExists = [&](const QString &candidateName) {
            return QFileInfo::exists(QDir(m_localPanel->currentPath()).filePath(candidateName));
        };
        FileItem remoteItem;
        remoteItem.name = remoteInfo.fileName().toStdString();
        remoteItem.path = remotePath.toStdString();
        remoteItem.type = FileItemType::File;
        const bool foundInPanel = session.panel != nullptr
            && session.panel->remoteItem(remotePath, &remoteItem);
        if (!foundInPanel)
        {
            remotePathExists(session, remotePath, &remoteItem);
        }

        TransferConflictDialog::Options options;
        options.direction = TransferConflictDialog::Direction::Download;
        options.source = remoteConflictDetails(remoteItem, remotePath);
        options.target = localConflictDetails(targetInfo);
        options.allowOverwrite = targetInfo.isFile() && !targetInfo.isSymLink();
        options.allowApplyToAll = false;
        options.suggestedName = availableConflictName(remoteInfo.fileName(), false, targetNameExists);

        while (true)
        {
            TransferConflictDialog::Decision decision;
            if (m_dialogsSuppressedForTesting)
            {
                decision.action = TransferConflictDialog::Action::Rename;
                decision.newName = options.suggestedName;
            }
            else
            {
                TransferConflictDialog dialog(options, this);
                dialog.exec();
                decision = dialog.decision();
            }

            if (decision.action == TransferConflictDialog::Action::Cancel)
            {
                appendLog("INFO", QString("用户取消下载：%1").arg(remotePath));
                return;
            }
            if (decision.action == TransferConflictDialog::Action::Skip)
            {
                appendLog("INFO", QString("已跳过下载，目标存在：%1").arg(localPath));
                return;
            }
            if (decision.action == TransferConflictDialog::Action::Overwrite)
            {
                replaceExisting = true;
                break;
            }

            const QString newName = decision.newName.trimmed();
            if (!panel_shared::isValidRemoteName(newName))
            {
                panel_shared::showInvalidRemoteNameWarning(this);
                options.suggestedName = newName;
                continue;
            }
            if (targetNameExists(newName))
            {
                panel_shared::showFileOperationWarning(this, "下载目标重名", QString("目标项目仍然存在：%1").arg(newName));
                options.suggestedName = newName;
                continue;
            }
            localPath = QDir(m_localPanel->currentPath()).filePath(newName);
            break;
        }
    }

    TransferJob job;
    job.id = makeTransferJobId("download");
    job.name = remoteInfo.fileName().toStdString();
    job.direction = TransferDirection::Download;
    job.status = TransferStatus::Pending;
    job.localPath = localPath.toStdString();
    job.remotePath = remotePath.toStdString();
    job.sessionId = session.id.toStdString();
    job.sessionName = session.displayName.toStdString();
    job.replaceExisting = replaceExisting;
    enqueueTransferJob(job);
    processTransferQueue();
}

/**
 * @brief 在当前远程会话中下载单个远程文件。
 * @param remotePath 远程文件路径。
 */
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

/**
 * @brief 下载远程目录，并创建目录传输父任务。
 * @param session 目标远程会话。
 * @param remotePath 远程目录路径。
 */
void MainWindow::downloadRemotePath(RemoteSession &session, const QString &remotePath)
{
    if (m_localPanel == nullptr || m_localPanel->currentPath().isEmpty())
    {
        showWarningMessage("下载失败", "本地目录不可用。");
        return;
    }

    QString localPath = QDir(m_localPanel->currentPath()).filePath(remoteBaseName(remotePath));
    bool replaceExisting = false;
    if (QFileInfo::exists(localPath))
    {
        const QFileInfo targetInfo(localPath);
        const auto targetNameExists = [&](const QString &candidateName) {
            return QFileInfo::exists(QDir(m_localPanel->currentPath()).filePath(candidateName));
        };
        FileItem remoteItem;
        remoteItem.name = remoteBaseName(remotePath).toStdString();
        remoteItem.path = remotePath.toStdString();
        remoteItem.type = FileItemType::Directory;
        const bool foundInPanel = session.panel != nullptr
            && session.panel->remoteItem(remotePath, &remoteItem);
        if (!foundInPanel)
        {
            remotePathExists(session, remotePath, &remoteItem);
        }

        TransferConflictDialog::Options options;
        options.direction = TransferConflictDialog::Direction::Download;
        options.source = remoteConflictDetails(remoteItem, remotePath);
        options.target = localConflictDetails(targetInfo);
        options.allowOverwrite = targetInfo.isDir() && !targetInfo.isSymLink();
        options.allowApplyToAll = false;
        options.suggestedName = availableConflictName(remoteBaseName(remotePath), true, targetNameExists);

        while (true)
        {
            TransferConflictDialog::Decision decision;
            if (m_dialogsSuppressedForTesting)
            {
                decision.action = TransferConflictDialog::Action::Rename;
                decision.newName = options.suggestedName;
            }
            else
            {
                TransferConflictDialog dialog(options, this);
                dialog.exec();
                decision = dialog.decision();
            }

            if (decision.action == TransferConflictDialog::Action::Cancel)
            {
                appendLog("INFO", QString("用户取消目录下载：%1").arg(remotePath));
                return;
            }
            if (decision.action == TransferConflictDialog::Action::Skip)
            {
                appendLog("INFO", QString("已跳过目录下载，目标存在：%1").arg(localPath));
                return;
            }
            if (decision.action == TransferConflictDialog::Action::Overwrite)
            {
                replaceExisting = true;
                break;
            }

            const QString newName = decision.newName.trimmed();
            if (!panel_shared::isValidRemoteName(newName))
            {
                panel_shared::showInvalidRemoteNameWarning(this);
                options.suggestedName = newName;
                continue;
            }
            if (targetNameExists(newName))
            {
                panel_shared::showFileOperationWarning(this, "下载目标重名", QString("目标项目仍然存在：%1").arg(newName));
                options.suggestedName = newName;
                continue;
            }
            localPath = QDir(m_localPanel->currentPath()).filePath(newName);
            break;
        }
    }
    if (replaceExisting)
    {
        TransferJob job;
        job.id = makeTransferJobId("download-directory-replacement");
        job.name = remoteBaseName(remotePath).toStdString();
        job.kind = TransferJobKind::DirectoryReplacement;
        job.direction = TransferDirection::Download;
        job.status = TransferStatus::Pending;
        job.localPath = localPath.toStdString();
        job.remotePath = remotePath.toStdString();
        job.sessionId = session.id.toStdString();
        job.sessionName = session.displayName.toStdString();
        job.replaceExisting = true;
        enqueueTransferJob(job);
        processTransferQueue();
        return;
    }
    const QString parentJobId = enqueueDirectoryTransferParent(
        TransferDirection::Download,
        remoteBaseName(remotePath),
        localPath,
        remotePath,
        session);
    startRemoteDirectoryDownloadPreparation(session, remotePath, localPath, parentJobId);
}

/**
 * @brief 异步扫描远程目录，并生成目录下载子任务。
 * @param session 目标远程会话。
 * @param remoteDirectoryPath 远程目录根路径。
 * @param localDirectoryPath 本地目录根路径。
 * @param parentJobId 目录父任务 id。
 */
void MainWindow::startRemoteDirectoryDownloadPreparation(RemoteSession &session, const QString &remoteDirectoryPath, const QString &localDirectoryPath, const QString &parentJobId)
{
    if (!session.connected || session.fileSystem == nullptr)
    {
        handlePreparedDirectoryTransfer(parentJobId, {}, "请先连接远程会话。");
        return;
    }

    const std::shared_ptr<RemoteFileSystem> fileSystem = session.fileSystem;
    const QString sessionId = session.id;
    const QString sessionName = session.displayName;
    QPointer<MainWindow> window(this);

    QThread *thread = QThread::create([window, fileSystem, sessionId, sessionName, remoteDirectoryPath, localDirectoryPath, parentJobId]() {
        std::vector<TransferJob> jobs;
        QString errorMessage;

        std::function<bool(const QString &, const QString &)> collectDirectory;
        collectDirectory = [&](const QString &remotePath, const QString &localPath) {
            std::vector<FileItem> children;
            try
            {
                children = fileSystem->listDirectory(remotePath.toStdString());
            }
            catch (const std::exception &exception)
            {
                errorMessage = QString::fromUtf8(exception.what());
                return false;
            }

            if (!QDir().mkpath(localPath))
            {
                errorMessage = QString("无法创建本地目录：%1").arg(localPath);
                return false;
            }

            for (const FileItem &child : children)
            {
                const QString childRemotePath = QString::fromStdString(child.path);
                const QString childLocalPath = QDir(localPath).filePath(QString::fromStdString(child.name));
                if (child.type == FileItemType::Directory)
                {
                    if (!collectDirectory(childRemotePath, childLocalPath))
                    {
                        return false;
                    }
                    continue;
                }
                if (child.type != FileItemType::File && child.type != FileItemType::Other)
                {
                    continue;
                }

                TransferJob job;
                job.id = makeTransferJobId("download");
                job.name = QString::fromStdString(child.name).toStdString();
                job.kind = TransferJobKind::File;
                job.parentId = parentJobId.toStdString();
                job.direction = TransferDirection::Download;
                job.status = TransferStatus::Pending;
                job.localPath = childLocalPath.toStdString();
                job.remotePath = childRemotePath.toStdString();
                job.sessionId = sessionId.toStdString();
                job.sessionName = sessionName.toStdString();
                job.totalBytes = child.size;
                job.transferredBytes = 0;
                jobs.push_back(std::move(job));
            }

            return true;
        };

        collectDirectory(remoteDirectoryPath, localDirectoryPath);

        if (window != nullptr)
        {
            QMetaObject::invokeMethod(window.data(), [window, parentJobId, jobs = std::move(jobs), errorMessage]() {
                if (window != nullptr)
                {
                    window->handlePreparedDirectoryTransfer(parentJobId, jobs, errorMessage);
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
 * @brief 接收目录上传或下载预处理结果，并将子任务加入队列。
 * @param parentJobId 目录父任务 id。
 * @param jobs 预处理生成的子文件任务。
 * @param errorMessage 预处理失败时的错误信息。
 */
void MainWindow::appendPreparedDirectoryTransferJobs(const QString &parentJobId, const std::vector<TransferJob> &jobs)
{
    const std::string parentJobIdString = parentJobId.toStdString();
    TransferJob *parent = m_transferQueue.find(parentJobIdString);
    if (parent == nullptr
        || parent->status == TransferStatus::Canceled
        || parent->status == TransferStatus::Canceling)
    {
        refreshTransferTable();
        return;
    }

    for (const TransferJob &job : jobs)
    {
        m_transferQueue.enqueue(job);
    }
    scheduleTransferTableRefresh();
    processTransferQueue();
}

void MainWindow::finishPreparedDirectoryTransfer(const QString &parentJobId, const QString &errorMessage)
{
    const std::string parentJobIdString = parentJobId.toStdString();
    TransferJob *parent = m_transferQueue.find(parentJobIdString);
    if (parent == nullptr
        || parent->status == TransferStatus::Canceled
        || parent->status == TransferStatus::Canceling)
    {
        refreshTransferTable();
        return;
    }

    parent->preparationFinished = true;
    if (!errorMessage.trimmed().isEmpty())
    {
        parent->preparationFailed = true;
        parent->status = TransferStatus::Failed;
        parent->finishedAtMs = currentEpochMillis();
        parent->errorMessage = errorMessage.toStdString();
        for (const TransferJob &snapshot : m_transferQueue.jobs())
        {
            if (snapshot.parentId == parentJobIdString)
            {
                m_transferQueue.cancel(snapshot.id, "目录准备失败，已停止后续传输");
                const auto cancelFlag = m_transferCancelFlags.find(snapshot.id);
                if (cancelFlag != m_transferCancelFlags.end() && cancelFlag->second != nullptr)
                {
                    cancelFlag->second->store(true);
                }
            }
        }
        refreshTransferTable();
        appendLog("ERROR", QString("目录传输准备失败：%1").arg(errorMessage));
        const bool localFailure = errorMessage.startsWith("本地") || errorMessage.startsWith("无法创建本地");
        showWarningMessage(
            parent->direction == TransferDirection::Upload ? "上传文件夹失败" : "下载文件夹失败",
            localFailure ? errorMessage : userFacingRemoteError(errorMessage));
        return;
    }

    const bool hasChildren = std::any_of(
        m_transferQueue.jobs().begin(),
        m_transferQueue.jobs().end(),
        [&parentJobIdString](const TransferJob &job) {
            return job.parentId == parentJobIdString;
        });
    if (!hasChildren)
    {
        parent->status = TransferStatus::Completed;
        parent->finishedAtMs = currentEpochMillis();
        parent->errorMessage = "0 / 0 个文件";
        if (parent->direction == TransferDirection::Upload)
        {
            RemoteSession *session = remoteSessionById(parent->sessionId);
            if (session != nullptr && session->connected)
            {
                loadRemotePath(*session, session->currentPath, false);
            }
        }
        else if (m_localPanel != nullptr)
        {
            m_localPanel->refresh();
        }
    }
    refreshTransferTable();
    processTransferQueue();
}

void MainWindow::handlePreparedDirectoryTransfer(const QString &parentJobId, const std::vector<TransferJob> &jobs, const QString &errorMessage)
{
    appendPreparedDirectoryTransferJobs(parentJobId, jobs);
    finishPreparedDirectoryTransfer(parentJobId, errorMessage);
}

/**
 * @brief 同步扫描远程目录并将下载子任务加入队列。
 * @param session 目标远程会话。
 * @param remoteDirectoryPath 远程目录根路径。
 * @param localDirectoryPath 本地目录根路径。
 * @param parentJobId 目录父任务 id。
 * @param errorMessage 可选错误输出。
 * @return 子任务入队成功返回 true。
 */
bool MainWindow::enqueueRemoteDirectoryDownload(RemoteSession &session, const QString &remoteDirectoryPath, const QString &localDirectoryPath, const QString &parentJobId, QString *errorMessage)
{
    if (!session.connected)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = "请先连接远程会话。";
        }
        return false;
    }

    std::vector<FileItem> children;
    try
    {
        children = session.fileSystem->listDirectory(remoteDirectoryPath.toStdString());
    }
    catch (const std::exception &error)
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString::fromUtf8(error.what());
        }
        return false;
    }

    if (!QDir().mkpath(localDirectoryPath))
    {
        if (errorMessage != nullptr)
        {
            *errorMessage = QString("无法创建本地目录：%1").arg(localDirectoryPath);
        }
        return false;
    }

    for (const FileItem &child : children)
    {
        const QString childRemotePath = QString::fromStdString(child.path);
        const QString childLocalPath = QDir(localDirectoryPath).filePath(QString::fromStdString(child.name));
        if (child.type == FileItemType::Directory)
        {
            if (!enqueueRemoteDirectoryDownload(session, childRemotePath, childLocalPath, parentJobId, errorMessage))
            {
                return false;
            }
            continue;
        }

        if (child.type != FileItemType::File && child.type != FileItemType::Other)
        {
            continue;
        }

        TransferJob job;
        job.id = makeTransferJobId("download");
        job.name = QString::fromStdString(child.name).toStdString();
        job.kind = TransferJobKind::File;
        job.parentId = parentJobId.toStdString();
        job.direction = TransferDirection::Download;
        job.status = TransferStatus::Pending;
        job.localPath = childLocalPath.toStdString();
        job.remotePath = childRemotePath.toStdString();
        job.sessionId = session.id.toStdString();
        job.sessionName = session.displayName.toStdString();
        job.totalBytes = child.size;
        enqueueTransferJob(job);
    }

    return true;
}
