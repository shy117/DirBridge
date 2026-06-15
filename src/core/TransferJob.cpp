#include "core/TransferJob.h"

#include <algorithm>

std::string toString(TransferDirection direction)
{
    switch (direction)
    {
    case TransferDirection::Upload:
        return "upload";
    case TransferDirection::Download:
        return "download";
    }

    return "upload";
}

std::string toString(TransferStatus status)
{
    switch (status)
    {
    case TransferStatus::Pending:
        return "pending";
    case TransferStatus::Running:
        return "running";
    case TransferStatus::Completed:
        return "completed";
    case TransferStatus::Failed:
        return "failed";
    case TransferStatus::Canceled:
        return "canceled";
    case TransferStatus::Canceling:
        return "canceling";
    }

    return "pending";
}

int progressPercent(const TransferJob &job)
{
    if (job.totalBytes <= 0)
    {
        return 0;
    }

    const std::int64_t clampedBytes = std::max<std::int64_t>(0, std::min(job.transferredBytes, job.totalBytes));
    return static_cast<int>((clampedBytes * 100) / job.totalBytes);
}
