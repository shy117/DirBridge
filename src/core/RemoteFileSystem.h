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

    virtual RemoteOperationResult createDirectory(const std::string &path)
    {
        (void)path;
        return {false, "create directory is not implemented"};
    }

    virtual RemoteOperationResult remove(const std::string &path)
    {
        (void)path;
        return {false, "remove is not implemented"};
    }

    virtual RemoteOperationResult rename(const std::string &sourcePath, const std::string &targetPath)
    {
        (void)sourcePath;
        (void)targetPath;
        return {false, "rename is not implemented"};
    }

    virtual RemoteOperationResult uploadFile(const std::string &localPath, const std::string &remotePath)
    {
        (void)localPath;
        (void)remotePath;
        return {false, "upload is not implemented"};
    }

    virtual RemoteOperationResult downloadFile(const std::string &remotePath, const std::string &localPath)
    {
        (void)remotePath;
        (void)localPath;
        return {false, "download is not implemented"};
    }
};

#endif // DIRBRIDGE_CORE_REMOTEFILESYSTEM_H
