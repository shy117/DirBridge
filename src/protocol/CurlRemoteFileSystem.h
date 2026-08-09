#ifndef DIRBRIDGE_PROTOCOL_CURLREMOTEFILESYSTEM_H
#define DIRBRIDGE_PROTOCOL_CURLREMOTEFILESYSTEM_H

#include "core/RemoteFileSystem.h"

#include <curl/curl.h>

#include <mutex>

class CurlRemoteFileSystem : public RemoteFileSystem
{
public:
    ~CurlRemoteFileSystem() override;
    RemoteOperationResult connect(const SiteProfile &profile) override;
    void disconnect() override;
    bool isConnected() const override;
    std::vector<FileItem> listDirectory(const std::string &path) override;
    RemoteOperationResult createDirectory(const std::string &path) override;
    RemoteOperationResult createFile(const std::string &path) override;
    RemoteOperationResult remove(const std::string &path) override;
    RemoteOperationResult removeFile(const std::string &path) override;
    RemoteOperationResult removeDirectory(const std::string &path) override;
    RemoteOperationResult rename(const std::string &sourcePath, const std::string &targetPath) override;
    RemoteOperationResult setPermissions(const std::string &path, int mode) override;
    RemoteOperationResult uploadFile(const std::string &localPath, const std::string &remotePath, TransferProgressCallback progress = {}) override;
    RemoteOperationResult downloadFile(const std::string &remotePath, const std::string &localPath, TransferProgressCallback progress = {}) override;

private:
    CURL *prepareHandleLocked(CURL *&handle);
    RemoteOperationResult performQuoteAtUrlLocked(const std::string &url, const std::vector<std::string> &commands);
    RemoteOperationResult performFtpCommandInDirectoryLocked(const std::string &directoryPath, const std::string &command);

    SiteProfile m_profile;
    bool m_connected = false;
    CURL *m_directoryHandle = nullptr;
    CURL *m_transferHandle = nullptr;
    mutable std::mutex m_directoryHandleMutex;
    mutable std::mutex m_transferHandleMutex;
};

#endif // DIRBRIDGE_PROTOCOL_CURLREMOTEFILESYSTEM_H
