#include "core/TransferQueue.h"

#include <algorithm>
#include <utility>

const TransferJob &TransferQueue::enqueue(TransferJob job)
{
    m_jobs.push_back(std::move(job));
    return m_jobs.back();
}

bool TransferQueue::update(const TransferJob &job)
{
    TransferJob *existing = find(job.id);
    if (existing == nullptr)
    {
        return false;
    }

    *existing = job;
    return true;
}

bool TransferQueue::cancel(const std::string &id, const std::string &message)
{
    TransferJob *job = find(id);
    if (job == nullptr
        || job->status == TransferStatus::Completed
        || job->status == TransferStatus::Failed
        || job->status == TransferStatus::Canceled
        || job->status == TransferStatus::Canceling)
    {
        return false;
    }

    job->status = job->status == TransferStatus::Running ? TransferStatus::Canceling : TransferStatus::Canceled;
    job->errorMessage = message;
    return true;
}

const TransferJob *TransferQueue::retry(const std::string &id, const std::string &retryId)
{
    const TransferJob *job = find(id);
    if (job == nullptr
        || job->kind != TransferJobKind::File
        || (job->status != TransferStatus::Failed && job->status != TransferStatus::Canceled))
    {
        return nullptr;
    }

    TransferJob retryJob = *job;
    retryJob.id = retryId;
    retryJob.status = TransferStatus::Pending;
    retryJob.transferredBytes = 0;
    retryJob.startedAtMs = 0;
    retryJob.finishedAtMs = 0;
    retryJob.lastProgressAtMs = 0;
    retryJob.lastProgressBytes = 0;
    retryJob.currentBytesPerSecond = 0.0;
    retryJob.errorMessage.clear();
    m_jobs.push_back(std::move(retryJob));
    return &m_jobs.back();
}

std::size_t TransferQueue::clearFinished()
{
    const auto originalSize = m_jobs.size();
    m_jobs.erase(std::remove_if(m_jobs.begin(), m_jobs.end(), [](const TransferJob &job) {
        return job.status == TransferStatus::Completed
            || job.status == TransferStatus::Failed
            || job.status == TransferStatus::Canceled;
    }), m_jobs.end());
    return originalSize - m_jobs.size();
}

TransferJob *TransferQueue::find(const std::string &id)
{
    const auto job = std::find_if(m_jobs.begin(), m_jobs.end(), [&id](const TransferJob &current) {
        return current.id == id;
    });
    return job == m_jobs.end() ? nullptr : &(*job);
}

const TransferJob *TransferQueue::find(const std::string &id) const
{
    const auto job = std::find_if(m_jobs.begin(), m_jobs.end(), [&id](const TransferJob &current) {
        return current.id == id;
    });
    return job == m_jobs.end() ? nullptr : &(*job);
}

TransferJob *TransferQueue::nextPending()
{
    const auto job = std::find_if(m_jobs.begin(), m_jobs.end(), [](const TransferJob &current) {
        return current.kind == TransferJobKind::File && current.status == TransferStatus::Pending;
    });
    return job == m_jobs.end() ? nullptr : &(*job);
}

std::size_t TransferQueue::runningCount() const
{
    return static_cast<std::size_t>(std::count_if(m_jobs.begin(), m_jobs.end(), [](const TransferJob &job) {
        return job.kind == TransferJobKind::File && job.status == TransferStatus::Running;
    }));
}

const std::vector<TransferJob> &TransferQueue::jobs() const
{
    return m_jobs;
}

void TransferQueue::clear()
{
    m_jobs.clear();
}
