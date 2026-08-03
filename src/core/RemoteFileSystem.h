#ifndef DIRBRIDGE_CORE_REMOTEFILESYSTEM_H
#define DIRBRIDGE_CORE_REMOTEFILESYSTEM_H

#include <string>
#include <vector>
#include <cstdint>
#include <functional>

#include "config/SiteProfile.h"
#include "core/FileItem.h"

struct RemoteOperationResult
{
    bool success = false;
    std::string message;
};

using TransferProgressCallback = std::function<bool(std::int64_t transferredBytes, std::int64_t totalBytes)>;

class RemoteFileSystem
{
public:
    virtual ~RemoteFileSystem() = default;

    virtual RemoteOperationResult connect(const SiteProfile &profile) = 0;
    virtual void disconnect() = 0;
    virtual bool isConnected() const = 0;
    virtual std::vector<FileItem> listDirectory(const std::string &path) = 0;

    /**
     * @brief 在远程文件系统上创建目录。
     * @param path 要创建的远程目录绝对路径。
     * @return 包含成功状态和后端消息的操作结果。
     */
    virtual RemoteOperationResult createDirectory(const std::string &path)
    {
        (void)path;
        return {false, "create directory is not implemented"};
    }

    /**
     * @brief 在远程文件系统上创建空文件。
     * @param path 要创建的远程文件绝对路径。
     * @return 包含成功状态和后端消息的操作结果。
     */
    virtual RemoteOperationResult createFile(const std::string &path)
    {
        (void)path;
        return {false, "create file is not implemented"};
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

    /**
     * @brief 修改远程文件或目录的 UNIX 权限位。
     * @param path 远程项目绝对路径。
     * @param mode 八进制权限值对应的整数，范围为 000 到 777。
     * @return 包含成功状态和后端消息的操作结果。
     */
    virtual RemoteOperationResult setPermissions(const std::string &path, int mode)
    {
        (void)path;
        (void)mode;
        return {false, "set permissions is not implemented"};
    }

    virtual RemoteOperationResult uploadFile(const std::string &localPath, const std::string &remotePath, TransferProgressCallback progress = {})
    {
        (void)localPath;
        (void)remotePath;
        (void)progress;
        return {false, "upload is not implemented"};
    }

    virtual RemoteOperationResult downloadFile(const std::string &remotePath, const std::string &localPath, TransferProgressCallback progress = {})
    {
        (void)remotePath;
        (void)localPath;
        (void)progress;
        return {false, "download is not implemented"};
    }
};

#endif // DIRBRIDGE_CORE_REMOTEFILESYSTEM_H
