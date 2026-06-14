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
    if (job == nullptr || job->status == TransferStatus::Completed || job->status == TransferStatus::Failed)
    {
        return false;
    }

    job->status = TransferStatus::Canceled;
    job->errorMessage = message;
    return true;
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
        return current.status == TransferStatus::Pending;
    });
    return job == m_jobs.end() ? nullptr : &(*job);
}

const std::vector<TransferJob> &TransferQueue::jobs() const
{
    return m_jobs;
}

void TransferQueue::clear()
{
    m_jobs.clear();
}
