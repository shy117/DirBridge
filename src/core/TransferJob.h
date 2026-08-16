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
    Preparing,
    Pending,
    Running,
    Completed,
    Failed,
    Canceled,
    Canceling
};

enum class TransferJobKind
{
    File,
    Directory,
    DirectoryReplacement,
    DirectoryEntry
};

struct TransferJob
{
    std::string id;
    std::string name;
    TransferJobKind kind = TransferJobKind::File;
    std::string parentId;
    TransferDirection direction = TransferDirection::Upload;
    TransferStatus status = TransferStatus::Pending;
    std::string localPath;
    std::string remotePath;
    std::string sessionId;
    std::string sessionName;
    std::int64_t totalBytes = -1;
    std::int64_t transferredBytes = 0;
    std::int64_t startedAtMs = 0;
    std::int64_t finishedAtMs = 0;
    std::int64_t lastProgressAtMs = 0;
    std::int64_t lastProgressBytes = 0;
    double currentBytesPerSecond = 0.0;
    int totalChildren = 0;
    int finishedChildren = 0;
    bool preparationFinished = true;
    bool preparationFailed = false;
    bool replaceExisting = false;
    bool externallyManaged = false;
    std::string errorMessage;
};

std::string toString(TransferJobKind kind);
std::string toString(TransferDirection direction);
std::string toString(TransferStatus status);
int progressPercent(const TransferJob &job);

#endif // DIRBRIDGE_CORE_TRANSFERJOB_H
