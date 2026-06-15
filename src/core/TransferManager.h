#ifndef DIRBRIDGE_CORE_TRANSFERMANAGER_H
#define DIRBRIDGE_CORE_TRANSFERMANAGER_H

#include "core/RemoteFileSystem.h"
#include "core/TransferQueue.h"

#include <cstddef>
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
     * @brief Sets the maximum number of jobs that may run at once.
     * @param limit Positive concurrency limit. Values below 1 are treated as 1.
     */
    void setConcurrencyLimit(std::size_t limit);

    /**
     * @brief Runs pending jobs while respecting the configured concurrency limit.
     */
    void processPending();

private:
    RemoteOperationResult runJob(const TransferJob &job);
    void notifyQueueChanged() const;

private:
    RemoteResolver m_remoteResolver;
    TransferQueue &m_queue;
    QueueChangedCallback m_queueChangedCallback;
    std::size_t m_concurrencyLimit = 1;
};

#endif // DIRBRIDGE_CORE_TRANSFERMANAGER_H
