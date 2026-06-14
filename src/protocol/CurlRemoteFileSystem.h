#ifndef DIRBRIDGE_PROTOCOL_CURLREMOTEFILESYSTEM_H
#define DIRBRIDGE_PROTOCOL_CURLREMOTEFILESYSTEM_H

#include "core/RemoteFileSystem.h"

class CurlRemoteFileSystem : public RemoteFileSystem
{
public:
    RemoteOperationResult connect(const SiteProfile &profile) override;
    void disconnect() override;
    bool isConnected() const override;
    std::vector<FileItem> listDirectory(const std::string &path) override;
    RemoteOperationResult createDirectory(const std::string &path) override;
    RemoteOperationResult createFile(const std::string &path) override;
    RemoteOperationResult remove(const std::string &path) override;
    RemoteOperationResult rename(const std::string &sourcePath, const std::string &targetPath) override;
    RemoteOperationResult uploadFile(const std::string &localPath, const std::string &remotePath) override;
    RemoteOperationResult downloadFile(const std::string &remotePath, const std::string &localPath) override;

private:
    SiteProfile m_profile;
    bool m_connected = false;
};

#endif // DIRBRIDGE_PROTOCOL_CURLREMOTEFILESYSTEM_H
