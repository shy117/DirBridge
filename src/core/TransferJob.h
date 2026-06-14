#ifndef DIRBRIDGE_CORE_TRANSFERJOB_H
#define DIRBRIDGE_CORE_TRANSFERJOB_H

#include <cstdint>
#include <string>

enum class TransferDirection
{
    Upload,
    Download
};

enum class TransferStatus
{
    Pending,
    Running,
    Completed,
    Failed,
    Canceled
};

struct TransferJob
{
    std::string id;
    std::string name;
    TransferDirection direction = TransferDirection::Upload;
    TransferStatus status = TransferStatus::Pending;
    std::string localPath;
    std::string remotePath;
    std::string sessionId;
    std::string sessionName;
    std::int64_t totalBytes = -1;
    std::int64_t transferredBytes = 0;
    std::string errorMessage;
};

std::string toString(TransferDirection direction);
std::string toString(TransferStatus status);
int progressPercent(const TransferJob &job);

#endif // DIRBRIDGE_CORE_TRANSFERJOB_H
