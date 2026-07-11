#include "core/ExternalEditDocument.h"

#include <algorithm>
#include <utility>

ExternalEditDocument::ExternalEditDocument(std::string sessionId,
                                           std::string remotePath,
                                           FileCacheEntry cacheEntry)
    : m_sessionId(std::move(sessionId))
    , m_remotePath(std::move(remotePath))
    , m_cacheEntry(std::move(cacheEntry))
{
}

const std::string &ExternalEditDocument::id() const
{
    return m_cacheEntry.documentId;
}

const std::string &ExternalEditDocument::sessionId() const
{
    return m_sessionId;
}

const std::string &ExternalEditDocument::remotePath() const
{
    return m_remotePath;
}

const FileCacheEntry &ExternalEditDocument::cacheEntry() const
{
    return m_cacheEntry;
}

ExternalEditState ExternalEditDocument::state() const
{
    return m_state;
}

std::uint64_t ExternalEditDocument::localVersion() const
{
    return m_localVersion;
}

std::uint64_t ExternalEditDocument::synchronizedVersion() const
{
    return m_synchronizedVersion;
}

const RemoteFileRevision &ExternalEditDocument::remoteRevision() const
{
    return m_remoteRevision;
}

void ExternalEditDocument::completeDownload(const RemoteFileRevision &revision)
{
    if (m_state != ExternalEditState::Downloading)
    {
        return;
    }
    m_remoteRevision = revision;
    m_state = ExternalEditState::EditingClean;
}

void ExternalEditDocument::failDownload()
{
    if (m_state == ExternalEditState::Downloading)
    {
        m_state = ExternalEditState::DownloadFailed;
    }
}

void ExternalEditDocument::failOpen()
{
    if (m_state == ExternalEditState::Downloading || m_state == ExternalEditState::EditingClean)
    {
        m_state = ExternalEditState::OpenFailed;
    }
}

void ExternalEditDocument::markLocalFileChanged()
{
    if (m_state == ExternalEditState::Closed)
    {
        return;
    }
    ++m_localVersion;
    if (!m_uploadingVersion.has_value())
    {
        m_state = ExternalEditState::PendingUpload;
    }
}

bool ExternalEditDocument::hasPendingUpload() const
{
    return m_localVersion > m_synchronizedVersion;
}

std::optional<std::uint64_t> ExternalEditDocument::beginUpload()
{
    if (m_state == ExternalEditState::Closed
        || m_state == ExternalEditState::Conflict
        || m_uploadingVersion.has_value()
        || !hasPendingUpload())
    {
        return std::nullopt;
    }

    m_uploadingVersion = m_localVersion;
    m_state = ExternalEditState::Uploading;
    return m_uploadingVersion;
}

bool ExternalEditDocument::completeUpload(std::uint64_t version, const RemoteFileRevision &revision)
{
    if (!m_uploadingVersion.has_value() || *m_uploadingVersion != version)
    {
        return false;
    }

    m_uploadingVersion.reset();
    m_synchronizedVersion = std::max(m_synchronizedVersion, version);
    m_remoteRevision = revision;
    m_state = hasPendingUpload() ? ExternalEditState::PendingUpload : ExternalEditState::EditingClean;
    return true;
}

bool ExternalEditDocument::failUpload(std::uint64_t version)
{
    if (!m_uploadingVersion.has_value() || *m_uploadingVersion != version)
    {
        return false;
    }

    m_uploadingVersion.reset();
    m_state = ExternalEditState::UploadFailed;
    return true;
}

void ExternalEditDocument::markFileUnavailable()
{
    if (m_state != ExternalEditState::Closed)
    {
        m_uploadingVersion.reset();
        m_state = ExternalEditState::UploadFailed;
    }
}

void ExternalEditDocument::markConflict()
{
    if (m_state == ExternalEditState::Closed)
    {
        return;
    }
    m_uploadingVersion.reset();
    m_state = ExternalEditState::Conflict;
}

void ExternalEditDocument::resolveConflictForOverwrite()
{
    if (m_state == ExternalEditState::Conflict)
    {
        m_state = hasPendingUpload() ? ExternalEditState::PendingUpload : ExternalEditState::EditingClean;
    }
}

void ExternalEditDocument::close()
{
    m_uploadingVersion.reset();
    m_state = ExternalEditState::Closed;
}
