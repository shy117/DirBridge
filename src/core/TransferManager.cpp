#include "core/TransferManager.h"

#include "core/FileReplacement.h"

#include <algorithm>
#include <utility>

TransferManager::TransferManager(RemoteFileSystem &remoteFileSystem, TransferQueue &queue)
    : m_remoteResolver([&remoteFileSystem](const TransferJob &) {
        return &remoteFileSystem;
    })
    , m_queue(queue)
{
}

TransferManager::TransferManager(RemoteResolver remoteResolver, TransferQueue &queue)
    : m_remoteResolver(std::move(remoteResolver))
    , m_queue(queue)
{
}

void TransferManager::setQueueChangedCallback(QueueChangedCallback callback)
{
    m_queueChangedCallback = std::move(callback);
}

void TransferManager::setProgressCallback(ProgressCallback callback)
{
    m_progressCallback = std::move(callback);
}

void TransferManager::setConcurrencyLimit(std::size_t limit)
{
    m_concurrencyLimit = std::max<std::size_t>(1, limit);
}

void TransferManager::processPending()
{
    while (m_queue.runningCount() < m_concurrencyLimit)
    {
        TransferJob *job = m_queue.nextPending();
        if (job == nullptr)
        {
            return;
        }

        job->status = TransferStatus::Running;
        job->errorMessage.clear();
        notifyQueueChanged();

        const RemoteOperationResult result = runJob(*job);
        if (job->status == TransferStatus::Canceling)
        {
            job->status = TransferStatus::Canceled;
            if (job->errorMessage.empty())
            {
                job->errorMessage = "transfer canceled";
            }
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
        notifyQueueChanged();
    }
}

RemoteOperationResult TransferManager::runJob(const TransferJob &job)
{
    RemoteFileSystem *remoteFileSystem = m_remoteResolver ? m_remoteResolver(job) : nullptr;
    if (remoteFileSystem == nullptr)
    {
        return {false, "remote session is not available"};
    }

    switch (job.direction)
    {
    case TransferDirection::Upload:
        if (job.kind == TransferJobKind::DirectoryReplacement)
        {
            if (!job.replaceExisting)
            {
                return {false, "directory replacement job must explicitly replace an existing target"};
            }
            return file_replacement::uploadDirectoryReplacing(
                *remoteFileSystem,
                job.localPath,
                job.remotePath,
                [this, &job](std::int64_t transferredBytes, std::int64_t totalBytes) {
                    return m_progressCallback ? m_progressCallback(job, transferredBytes, totalBytes) : true;
                });
        }
        if (job.replaceExisting)
        {
            return file_replacement::uploadFileReplacing(
                *remoteFileSystem,
                job.localPath,
                job.remotePath,
                [this, &job](std::int64_t transferredBytes, std::int64_t totalBytes) {
                    return m_progressCallback ? m_progressCallback(job, transferredBytes, totalBytes) : true;
                });
        }
        return remoteFileSystem->uploadFile(job.localPath, job.remotePath, [this, &job](std::int64_t transferredBytes, std::int64_t totalBytes) {
            return m_progressCallback ? m_progressCallback(job, transferredBytes, totalBytes) : true;
        });
    case TransferDirection::Download:
        if (job.kind == TransferJobKind::DirectoryReplacement)
        {
            if (!job.replaceExisting)
            {
                return {false, "directory replacement job must explicitly replace an existing target"};
            }
            return file_replacement::downloadDirectoryReplacing(
                *remoteFileSystem,
                job.remotePath,
                job.localPath,
                [this, &job](std::int64_t transferredBytes, std::int64_t totalBytes) {
                    return m_progressCallback ? m_progressCallback(job, transferredBytes, totalBytes) : true;
                });
        }
        if (job.replaceExisting)
        {
            return file_replacement::downloadFileReplacing(
                *remoteFileSystem,
                job.remotePath,
                job.localPath,
                [this, &job](std::int64_t transferredBytes, std::int64_t totalBytes) {
                    return m_progressCallback ? m_progressCallback(job, transferredBytes, totalBytes) : true;
                });
        }
        return remoteFileSystem->downloadFile(job.remotePath, job.localPath, [this, &job](std::int64_t transferredBytes, std::int64_t totalBytes) {
            return m_progressCallback ? m_progressCallback(job, transferredBytes, totalBytes) : true;
        });
    }

    return {false, "unsupported transfer direction"};
}

void TransferManager::notifyQueueChanged() const
{
    if (m_queueChangedCallback)
    {
        m_queueChangedCallback();
    }
}
