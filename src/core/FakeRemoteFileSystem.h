#ifndef DIRBRIDGE_CORE_FAKEREMOTEFILESYSTEM_H
#define DIRBRIDGE_CORE_FAKEREMOTEFILESYSTEM_H

#include "core/RemoteFileSystem.h"

#include <map>

class FakeRemoteFileSystem : public RemoteFileSystem
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
    bool m_connected = false;
    SiteProfile m_profile;
    std::map<std::string, FileItem> m_items;
};

#endif // DIRBRIDGE_CORE_FAKEREMOTEFILESYSTEM_H
