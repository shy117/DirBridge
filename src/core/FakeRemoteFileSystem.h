#ifndef DIRBRIDGE_CORE_FAKEREMOTEFILESYSTEM_H
#define DIRBRIDGE_CORE_FAKEREMOTEFILESYSTEM_H

#include "core/RemoteFileSystem.h"

class FakeRemoteFileSystem : public RemoteFileSystem
{
public:
    RemoteOperationResult connect(const SiteProfile &profile) override;
    void disconnect() override;
    bool isConnected() const override;
    std::vector<FileItem> listDirectory(const std::string &path) override;

private:
    bool m_connected = false;
    SiteProfile m_profile;
};

#endif // DIRBRIDGE_CORE_FAKEREMOTEFILESYSTEM_H
