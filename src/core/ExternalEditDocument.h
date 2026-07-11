#ifndef DIRBRIDGE_CORE_EXTERNALEDITDOCUMENT_H
#define DIRBRIDGE_CORE_EXTERNALEDITDOCUMENT_H

#include <cstdint>
#include <optional>
#include <string>

#include "core/FileCache.h"

/**
 * @brief 用于尽力检测远程文件变化的基线信息。
 */
struct RemoteFileRevision
{
    std::int64_t size = -1;
    std::string modifiedTime;
    bool reliable = false;
};

/**
 * @brief 外部编辑会话的可见状态。
 */
enum class ExternalEditState
{
    Downloading,
    DownloadFailed,
    OpenFailed,
    EditingClean,
    PendingUpload,
    Uploading,
    UploadFailed,
    Conflict,
    Closed
};

/**
 * @brief 单个远程文件外部编辑会话的权威状态。
 */
class ExternalEditDocument
{
public:
    ExternalEditDocument(std::string sessionId,
                         std::string remotePath,
                         FileCacheEntry cacheEntry);

    const std::string &id() const;
    const std::string &sessionId() const;
    const std::string &remotePath() const;
    const FileCacheEntry &cacheEntry() const;
    ExternalEditState state() const;
    std::uint64_t localVersion() const;
    std::uint64_t synchronizedVersion() const;
    const RemoteFileRevision &remoteRevision() const;

    void completeDownload(const RemoteFileRevision &revision);
    void failDownload();
    void failOpen();
    void markLocalFileChanged();
    bool hasPendingUpload() const;
    std::optional<std::uint64_t> beginUpload();
    bool completeUpload(std::uint64_t version, const RemoteFileRevision &revision);
    bool failUpload(std::uint64_t version);
    void markFileUnavailable();
    void markConflict();
    void resolveConflictForOverwrite();
    void close();

private:
    std::string m_sessionId;
    std::string m_remotePath;
    FileCacheEntry m_cacheEntry;
    RemoteFileRevision m_remoteRevision;
    ExternalEditState m_state = ExternalEditState::Downloading;
    std::uint64_t m_localVersion = 0;
    std::uint64_t m_synchronizedVersion = 0;
    std::optional<std::uint64_t> m_uploadingVersion;
};

#endif // DIRBRIDGE_CORE_EXTERNALEDITDOCUMENT_H
