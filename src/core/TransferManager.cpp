#include "core/TransferManager.h"

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

void TransferManager::processPending()
{
    while (TransferJob *job = m_queue.nextPending())
    {
        job->status = TransferStatus::Running;
        job->errorMessage.clear();
        notifyQueueChanged();

        const RemoteOperationResult result = runJob(*job);
        if (result.success)
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
        return remoteFileSystem->uploadFile(job.localPath, job.remotePath);
    case TransferDirection::Download:
        return remoteFileSystem->downloadFile(job.remotePath, job.localPath);
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
