#ifndef DIRBRIDGE_CORE_TRANSFERQUEUE_H
#define DIRBRIDGE_CORE_TRANSFERQUEUE_H

#include "core/TransferJob.h"

#include <cstddef>
#include <string>
#include <vector>

class TransferQueue
{
public:
    /**
     * @brief Adds a transfer job to the end of the queue.
     * @param job Job definition to enqueue.
     * @return Enqueued job snapshot.
     */
    const TransferJob &enqueue(TransferJob job);

    /**
     * @brief Replaces an existing job with the same id.
     * @param job Updated job snapshot.
     * @return true when an existing job was updated.
     */
    bool update(const TransferJob &job);

    /**
     * @brief Marks a pending or running job as canceled.
     * @param id Job id to cancel.
     * @param message Optional cancellation reason.
     * @return true when the job exists and can be canceled.
     */
    bool cancel(const std::string &id, const std::string &message = {});

    /**
     * @brief Enqueues a retry copy of a failed or canceled job.
     * @param id Original job id to retry.
     * @param retryId Unique id for the new retry job.
     * @return Pointer to the new pending job, or nullptr when retry is not allowed.
     */
    const TransferJob *retry(const std::string &id, const std::string &retryId);

    /**
     * @brief Removes completed, failed, and canceled jobs from history.
     * @return Number of jobs removed.
     */
    std::size_t clearFinished();

    /**
     * @brief Finds a mutable job by id.
     * @param id Job id to search.
     * @return Pointer to the job, or nullptr when not found.
     */
    TransferJob *find(const std::string &id);

    /**
     * @brief Finds an immutable job by id.
     * @param id Job id to search.
     * @return Pointer to the job, or nullptr when not found.
     */
    const TransferJob *find(const std::string &id) const;

    /**
     * @brief Finds the next job waiting for execution.
     * @return Pointer to the next pending job, or nullptr when the queue is idle.
     */
    TransferJob *nextPending();

    /**
     * @brief Counts jobs currently in the running state.
     * @return Number of running jobs.
     */
    std::size_t runningCount() const;

    /**
     * @brief Returns all jobs in display order.
     * @return Queue snapshots ordered by insertion.
     */
    const std::vector<TransferJob> &jobs() const;

    /**
     * @brief Removes every job from the queue.
     */
    void clear();

private:
    std::vector<TransferJob> m_jobs;
};

#endif // DIRBRIDGE_CORE_TRANSFERQUEUE_H
