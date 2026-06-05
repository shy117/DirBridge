#ifndef DIRBRIDGE_CORE_REMOTEFILESYSTEM_H
#define DIRBRIDGE_CORE_REMOTEFILESYSTEM_H

#include <string>
#include <vector>

#include "config/SiteProfile.h"
#include "core/FileItem.h"

struct RemoteOperationResult
{
    bool success = false;
    std::string message;
};

class RemoteFileSystem
{
public:
    virtual ~RemoteFileSystem() = default;

    virtual RemoteOperationResult connect(const SiteProfile &profile) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual std::vector<FileItem> listDirectory(const std::string &path) = 0;
};

#endif // DIRBRIDGE_CORE_REMOTEFILESYSTEM_H
