#ifndef DIRBRIDGE_CORE_TRANSFERMANAGER_H
#define DIRBRIDGE_CORE_TRANSFERMANAGER_H

#include "core/RemoteFileSystem.h"
#include "core/TransferQueue.h"

#include <functional>

class TransferManager
{
public:
    using QueueChangedCallback = std::function<void()>;
    using RemoteResolver = std::function<RemoteFileSystem *(const TransferJob &)>;

    /**
     * @brief Creates a manager that schedules jobs from a transfer queue.
     * @param remoteFileSystem Connected remote filesystem used to execute transfers.
     * @param queue Queue containing pending and historical transfer jobs.
     */
    TransferManager(RemoteFileSystem &remoteFileSystem, TransferQueue &queue);

    /**
     * @brief Creates a manager that resolves the remote filesystem per job.
     * @param remoteResolver Resolver returning the backend for each transfer job.
     * @param queue Queue containing pending and historical transfer jobs.
     */
    TransferManager(RemoteResolver remoteResolver, TransferQueue &queue);

    /**
     * @brief Sets a callback emitted whenever job state changes.
     * @param callback Callback used by UI adapters to refresh presentation state.
     */
    void setQueueChangedCallback(QueueChangedCallback callback);

    /**
     * @brief Runs all pending jobs sequentially.
     *
     * V1 uses a single-worker scheduler. Later versions can keep this API and
     * add cancellation, retry, and concurrency limits behind the manager.
     */
    void processPending();

private:
    RemoteOperationResult runJob(const TransferJob &job);
    void notifyQueueChanged() const;

private:
    RemoteResolver m_remoteResolver;
    TransferQueue &m_queue;
    QueueChangedCallback m_queueChangedCallback;
};

#endif // DIRBRIDGE_CORE_TRANSFERMANAGER_H
