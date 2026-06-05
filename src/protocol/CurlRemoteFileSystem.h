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

private:
    SiteProfile m_profile;
    bool m_connected = false;
};

#endif // DIRBRIDGE_PROTOCOL_CURLREMOTEFILESYSTEM_H
