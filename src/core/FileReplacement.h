#ifndef DIRBRIDGE_CORE_FILE_REPLACEMENT_H
#define DIRBRIDGE_CORE_FILE_REPLACEMENT_H

#include "core/RemoteFileSystem.h"

#include <string>

namespace file_replacement
{
/**
 * @brief 将本地文件上传到远程临时文件，并在成功后安全替换已有远程文件。
 * @param remoteFileSystem 已连接的远程文件系统。
 * @param localPath 本地源文件路径。
 * @param remotePath 已存在的远程目标文件路径。
 * @param progress 上传进度与取消回调。
 * @return 替换、回滚和临时对象清理的完整结果。
 */
RemoteOperationResult uploadFileReplacing(
    RemoteFileSystem &remoteFileSystem,
    const std::string &localPath,
    const std::string &remotePath,
    TransferProgressCallback progress = {});

/**
 * @brief 将本地目录完整上传到远程同级临时目录，成功后整体替换既有远程目录。
 * @param remoteFileSystem 已连接的远程文件系统。
 * @param localDirectoryPath 本地源目录。
 * @param remoteDirectoryPath 必须已经存在的远程目标目录。
 * @param progress 汇报所有文件累计字节数的进度回调，返回 false 时取消。
 * @return 替换、回滚和清理结果。
 */
RemoteOperationResult uploadDirectoryReplacing(
    RemoteFileSystem &remoteFileSystem,
    const std::string &localDirectoryPath,
    const std::string &remoteDirectoryPath,
    TransferProgressCallback progress = {});

/**
 * @brief 将远程目录完整下载到本地同级临时目录，成功后整体替换既有本地目录。
 * @param remoteFileSystem 已连接的远程文件系统。
 * @param remoteDirectoryPath 远程源目录。
 * @param localDirectoryPath 必须已经存在的本地目标目录。
 * @param progress 汇报所有文件累计字节数的进度回调，返回 false 时取消。
 * @return 替换、回滚和清理结果。
 */
RemoteOperationResult downloadDirectoryReplacing(
    RemoteFileSystem &remoteFileSystem,
    const std::string &remoteDirectoryPath,
    const std::string &localDirectoryPath,
    TransferProgressCallback progress = {});

/**
 * @brief 将远程文件下载到本地临时文件，并在成功后安全替换已有本地文件。
 * @param remoteFileSystem 已连接的远程文件系统。
 * @param remotePath 远程源文件路径。
 * @param localPath 已存在的本地目标文件路径。
 * @param progress 下载进度与取消回调。
 * @return 替换、回滚和临时文件清理的完整结果。
 */
RemoteOperationResult downloadFileReplacing(
    RemoteFileSystem &remoteFileSystem,
    const std::string &remotePath,
    const std::string &localPath,
    TransferProgressCallback progress = {});
}

#endif // DIRBRIDGE_CORE_FILE_REPLACEMENT_H
