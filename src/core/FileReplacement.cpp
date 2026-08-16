#include "core/FileReplacement.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <exception>
#include <filesystem>
#include <limits>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>

namespace
{
std::string normalizeRemotePath(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty() || path.front() != '/')
    {
        path.insert(path.begin(), '/');
    }
    while (path.size() > 1 && path.back() == '/')
    {
        path.pop_back();
    }
    return path;
}

std::string remoteParentPath(const std::string &path)
{
    const std::string normalized = normalizeRemotePath(path);
    const std::size_t slashIndex = normalized.find_last_of('/');
    return slashIndex == 0 ? "/" : normalized.substr(0, slashIndex);
}

std::string remoteBaseName(const std::string &path)
{
    const std::string normalized = normalizeRemotePath(path);
    return normalized == "/" ? std::string() : normalized.substr(normalized.find_last_of('/') + 1);
}

std::string joinRemotePath(const std::string &parent, const std::string &name)
{
    return parent == "/" ? "/" + name : parent + "/" + name;
}

std::string transactionToken()
{
    static std::atomic_uint64_t sequence{0};
    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    std::ostringstream stream;
    stream << std::hex << now << '-' << sequence.fetch_add(1, std::memory_order_relaxed);
    return stream.str();
}

std::string uniqueRemoteArtifactPath(
    const std::string &parent,
    const std::string &role,
    const std::set<std::string> &existingNames)
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const std::string name = ".dirbridge-" + role + '-' + transactionToken() + ".tmp";
        if (existingNames.find(name) == existingNames.end())
        {
            return joinRemotePath(parent, name);
        }
    }
    return {};
}

bool containsRemotePath(const std::vector<FileItem> &items, const std::string &path)
{
    const std::string normalized = normalizeRemotePath(path);
    return std::any_of(items.begin(), items.end(), [&normalized](const FileItem &item) {
        return normalizeRemotePath(item.path) == normalized;
    });
}

RemoteOperationResult cleanupRemoteFile(RemoteFileSystem &remoteFileSystem, const std::string &path)
{
    try
    {
        if (!containsRemotePath(remoteFileSystem.listDirectory(remoteParentPath(path)), path))
        {
            return {true, "remote artifact is absent"};
        }
    }
    catch (...)
    {
    }
    return remoteFileSystem.removeFile(path);
}

RemoteOperationResult cleanupRemoteTree(RemoteFileSystem &remoteFileSystem, const std::string &path)
{
    std::vector<FileItem> parentItems;
    try
    {
        parentItems = remoteFileSystem.listDirectory(remoteParentPath(path));
    }
    catch (const std::exception &error)
    {
        return {false, std::string("failed to inspect remote directory artifact at ") + path + ": " + error.what()};
    }

    const std::string normalized = normalizeRemotePath(path);
    const auto artifact = std::find_if(parentItems.begin(), parentItems.end(), [&normalized](const FileItem &item) {
        return normalizeRemotePath(item.path) == normalized;
    });
    if (artifact == parentItems.end())
    {
        return {true, "remote directory artifact is absent"};
    }
    if (artifact->type != FileItemType::Directory)
    {
        return remoteFileSystem.removeFile(path);
    }

    std::vector<FileItem> children;
    try
    {
        children = remoteFileSystem.listDirectory(path);
    }
    catch (const std::exception &error)
    {
        return {false, std::string("failed to list remote directory artifact at ") + path + ": " + error.what()};
    }

    for (const FileItem &child : children)
    {
        const RemoteOperationResult childResult = child.type == FileItemType::Directory
            ? cleanupRemoteTree(remoteFileSystem, child.path)
            : remoteFileSystem.removeFile(child.path);
        if (!childResult.success)
        {
            return {false, "failed to clean remote directory child at " + child.path + ": " + childResult.message};
        }
    }
    return remoteFileSystem.removeDirectory(path);
}

std::string localPathText(const std::filesystem::path &path)
{
    return path.u8string();
}

std::filesystem::path uniqueLocalArtifactPath(
    const std::filesystem::path &parent,
    const std::string &role)
{
    for (int attempt = 0; attempt < 100; ++attempt)
    {
        const std::filesystem::path candidate = parent
            / std::filesystem::u8path(".dirbridge-" + role + '-' + transactionToken() + ".tmp");
        std::error_code error;
        if (!std::filesystem::exists(candidate, error) && !error)
        {
            return candidate;
        }
    }
    return {};
}

RemoteOperationResult cleanupLocalFile(const std::filesystem::path &path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        return error
            ? RemoteOperationResult{false, "failed to inspect local artifact at " + localPathText(path) + ": " + error.message()}
            : RemoteOperationResult{true, "local artifact is absent"};
    }
    if (!std::filesystem::remove(path, error) || error)
    {
        return {false, "failed to remove local artifact at " + localPathText(path) + ": " + error.message()};
    }
    return {true, "local artifact removed"};
}

RemoteOperationResult cleanupLocalTree(const std::filesystem::path &path)
{
    std::error_code error;
    if (!std::filesystem::exists(path, error))
    {
        return error
            ? RemoteOperationResult{false, "failed to inspect local directory artifact at "
                    + localPathText(path) + ": " + error.message()}
            : RemoteOperationResult{true, "local directory artifact is absent"};
    }
    std::filesystem::remove_all(path, error);
    if (error)
    {
        return {false, "failed to remove local directory artifact at "
                + localPathText(path) + ": " + error.message()};
    }
    return {true, "local directory artifact removed"};
}

bool isValidRemoteChildName(const std::string &name)
{
    return !name.empty()
        && name != "."
        && name != ".."
        && name.find('/') == std::string::npos
        && name.find('\\') == std::string::npos;
}
}

namespace file_replacement
{
RemoteOperationResult uploadFileReplacing(
    RemoteFileSystem &remoteFileSystem,
    const std::string &localPath,
    const std::string &remotePath,
    TransferProgressCallback progress)
{
    const std::string normalizedTarget = normalizeRemotePath(remotePath);
    const std::string parent = remoteParentPath(normalizedTarget);
    const std::string targetName = remoteBaseName(normalizedTarget);
    if (targetName.empty())
    {
        return {false, "remote replacement target must be a file"};
    }

    std::vector<FileItem> siblings;
    try
    {
        siblings = remoteFileSystem.listDirectory(parent);
    }
    catch (const std::exception &error)
    {
        return {false, std::string("failed to inspect remote replacement target: ") + error.what()};
    }

    const auto target = std::find_if(siblings.begin(), siblings.end(), [&normalizedTarget](const FileItem &item) {
        return normalizeRemotePath(item.path) == normalizedTarget;
    });
    if (target == siblings.end())
    {
        return {false, "remote replacement target does not exist: " + normalizedTarget};
    }
    if (target->type != FileItemType::File)
    {
        return {false, "remote replacement target is not a regular file: " + normalizedTarget};
    }

    std::set<std::string> existingNames;
    for (const FileItem &item : siblings)
    {
        existingNames.insert(item.name);
    }
    const std::string temporaryPath = uniqueRemoteArtifactPath(parent, "upload", existingNames);
    const std::string backupPath = uniqueRemoteArtifactPath(parent, "backup", existingNames);
    if (temporaryPath.empty() || backupPath.empty() || temporaryPath == backupPath)
    {
        return {false, "failed to allocate unique remote replacement paths"};
    }

    const RemoteOperationResult uploadResult = remoteFileSystem.uploadFile(localPath, temporaryPath, std::move(progress));
    if (!uploadResult.success)
    {
        const RemoteOperationResult cleanupResult = cleanupRemoteFile(remoteFileSystem, temporaryPath);
        std::string message = "temporary remote upload failed at " + temporaryPath + ": " + uploadResult.message;
        if (!cleanupResult.success)
        {
            message += "; temporary artifact cleanup failed at " + temporaryPath + ": " + cleanupResult.message;
        }
        return {false, message};
    }

    const RemoteOperationResult backupResult = remoteFileSystem.rename(normalizedTarget, backupPath);
    if (!backupResult.success)
    {
        const RemoteOperationResult cleanupResult = cleanupRemoteFile(remoteFileSystem, temporaryPath);
        std::string message = "failed to move original remote target to backup " + backupPath + ": " + backupResult.message;
        if (!cleanupResult.success)
        {
            message += "; temporary artifact cleanup failed at " + temporaryPath + ": " + cleanupResult.message;
        }
        return {false, message};
    }

    const RemoteOperationResult replaceResult = remoteFileSystem.rename(temporaryPath, normalizedTarget);
    if (!replaceResult.success)
    {
        const RemoteOperationResult restoreResult = remoteFileSystem.rename(backupPath, normalizedTarget);
        const RemoteOperationResult cleanupResult = cleanupRemoteFile(remoteFileSystem, temporaryPath);
        std::string message;
        if (restoreResult.success)
        {
            message = "remote replacement failed and original target was restored from " + backupPath + ": " + replaceResult.message;
        }
        else
        {
            message = "remote replacement failed and original target could not be restored from " + backupPath
                + ": " + restoreResult.message + "; replacement error: " + replaceResult.message;
        }
        if (!cleanupResult.success)
        {
            message += "; temporary artifact cleanup failed at " + temporaryPath + ": " + cleanupResult.message;
        }
        return {false, message};
    }

    const RemoteOperationResult cleanupResult = cleanupRemoteFile(remoteFileSystem, backupPath);
    if (!cleanupResult.success)
    {
        return {false, "remote replacement succeeded but backup cleanup failed at " + backupPath + ": " + cleanupResult.message};
    }
    return {true, "remote file replaced safely"};
}

RemoteOperationResult uploadDirectoryReplacing(
    RemoteFileSystem &remoteFileSystem,
    const std::string &localDirectoryPath,
    const std::string &remoteDirectoryPath,
    TransferProgressCallback progress)
{
    const std::filesystem::path sourcePath = std::filesystem::u8path(localDirectoryPath);
    std::error_code localError;
    const std::filesystem::file_status sourceStatus = std::filesystem::symlink_status(sourcePath, localError);
    if (localError || !std::filesystem::is_directory(sourceStatus))
    {
        return {false, "local directory replacement source is not a regular directory: " + localDirectoryPath};
    }

    struct LocalEntry
    {
        std::filesystem::path path;
        std::string relativePath;
        bool directory = false;
        std::int64_t size = 0;
    };

    std::vector<LocalEntry> localEntries;
    std::int64_t totalBytes = 0;
    std::filesystem::recursive_directory_iterator iterator(sourcePath, localError);
    const std::filesystem::recursive_directory_iterator end;
    while (!localError && iterator != end)
    {
        const std::filesystem::path entryPath = iterator->path();
        const std::filesystem::file_status entryStatus = std::filesystem::symlink_status(entryPath, localError);
        if (localError)
        {
            break;
        }
        if (std::filesystem::is_symlink(entryStatus))
        {
            return {false, "symbolic links are not supported in directory replacement: " + localPathText(entryPath)};
        }

        LocalEntry entry;
        entry.path = entryPath;
        entry.relativePath = std::filesystem::relative(entryPath, sourcePath, localError).generic_u8string();
        if (localError)
        {
            break;
        }
        entry.directory = std::filesystem::is_directory(entryStatus);
        if (!entry.directory)
        {
            if (!std::filesystem::is_regular_file(entryStatus))
            {
                return {false, "unsupported local directory entry: " + localPathText(entryPath)};
            }
            const std::uintmax_t size = std::filesystem::file_size(entryPath, localError);
            if (localError || size > static_cast<std::uintmax_t>(std::numeric_limits<std::int64_t>::max()))
            {
                break;
            }
            entry.size = static_cast<std::int64_t>(size);
            if (entry.size > std::numeric_limits<std::int64_t>::max() - totalBytes)
            {
                return {false, "local directory replacement source is too large to report progress: " + localDirectoryPath};
            }
            totalBytes += entry.size;
        }
        localEntries.push_back(std::move(entry));
        iterator.increment(localError);
    }
    if (localError)
    {
        return {false, "failed to inspect local directory replacement source at " + localDirectoryPath + ": " + localError.message()};
    }

    const std::string normalizedTarget = normalizeRemotePath(remoteDirectoryPath);
    const std::string parent = remoteParentPath(normalizedTarget);
    const std::string targetName = remoteBaseName(normalizedTarget);
    if (targetName.empty())
    {
        return {false, "remote directory replacement target must not be the root directory"};
    }

    std::vector<FileItem> siblings;
    try
    {
        siblings = remoteFileSystem.listDirectory(parent);
    }
    catch (const std::exception &error)
    {
        return {false, std::string("failed to inspect remote directory replacement target: ") + error.what()};
    }
    const auto target = std::find_if(siblings.begin(), siblings.end(), [&normalizedTarget](const FileItem &item) {
        return normalizeRemotePath(item.path) == normalizedTarget;
    });
    if (target == siblings.end())
    {
        return {false, "remote directory replacement target does not exist: " + normalizedTarget};
    }
    if (target->type != FileItemType::Directory)
    {
        return {false, "remote directory replacement target is not a directory: " + normalizedTarget};
    }

    std::set<std::string> existingNames;
    for (const FileItem &item : siblings)
    {
        existingNames.insert(item.name);
    }
    const std::string temporaryPath = uniqueRemoteArtifactPath(parent, "upload-directory", existingNames);
    const std::string backupPath = uniqueRemoteArtifactPath(parent, "backup-directory", existingNames);
    if (temporaryPath.empty() || backupPath.empty() || temporaryPath == backupPath)
    {
        return {false, "failed to allocate unique remote directory replacement paths"};
    }

    const RemoteOperationResult createTemporaryResult = remoteFileSystem.createDirectory(temporaryPath);
    if (!createTemporaryResult.success)
    {
        return {false, "failed to create temporary remote directory at " + temporaryPath + ": " + createTemporaryResult.message};
    }

    const auto failAndCleanTemporary = [&](const std::string &message) {
        const RemoteOperationResult cleanupResult = cleanupRemoteTree(remoteFileSystem, temporaryPath);
        return cleanupResult.success
            ? RemoteOperationResult{false, message}
            : RemoteOperationResult{false, message + "; temporary directory cleanup failed at "
                    + temporaryPath + ": " + cleanupResult.message};
    };

    std::int64_t completedBytes = 0;
    const auto notifyProgress = [&](std::int64_t transferredBytes) {
        return progress ? progress(transferredBytes, totalBytes) : true;
    };
    if (!notifyProgress(0))
    {
        return failAndCleanTemporary("remote directory replacement was canceled before upload");
    }

    for (const LocalEntry &entry : localEntries)
    {
        if (!notifyProgress(completedBytes))
        {
            return failAndCleanTemporary("remote directory replacement was canceled during upload");
        }
        const std::string remoteEntryPath = joinRemotePath(temporaryPath, entry.relativePath);
        if (entry.directory)
        {
            const RemoteOperationResult createResult = remoteFileSystem.createDirectory(remoteEntryPath);
            if (!createResult.success)
            {
                return failAndCleanTemporary(
                    "failed to create temporary remote subdirectory at " + remoteEntryPath + ": " + createResult.message);
            }
            continue;
        }

        const RemoteOperationResult uploadResult = remoteFileSystem.uploadFile(
            localPathText(entry.path),
            remoteEntryPath,
            [&](std::int64_t fileBytes, std::int64_t) {
                return notifyProgress(completedBytes + std::max<std::int64_t>(0, fileBytes));
            });
        if (!uploadResult.success)
        {
            return failAndCleanTemporary(
                "failed to upload temporary remote directory file at " + remoteEntryPath + ": " + uploadResult.message);
        }
        completedBytes += entry.size;
    }
    if (!notifyProgress(totalBytes))
    {
        return failAndCleanTemporary("remote directory replacement was canceled after upload");
    }

    const RemoteOperationResult backupResult = remoteFileSystem.rename(normalizedTarget, backupPath);
    if (!backupResult.success)
    {
        return failAndCleanTemporary(
            "failed to move original remote directory to backup " + backupPath + ": " + backupResult.message);
    }

    const RemoteOperationResult replaceResult = remoteFileSystem.rename(temporaryPath, normalizedTarget);
    if (!replaceResult.success)
    {
        const RemoteOperationResult restoreResult = remoteFileSystem.rename(backupPath, normalizedTarget);
        const RemoteOperationResult cleanupResult = cleanupRemoteTree(remoteFileSystem, temporaryPath);
        std::string message = restoreResult.success
            ? "remote directory replacement failed and original directory was restored from " + backupPath
                + ": " + replaceResult.message
            : "remote directory replacement failed and original directory could not be restored from " + backupPath
                + ": " + restoreResult.message + "; replacement error: " + replaceResult.message;
        if (!cleanupResult.success)
        {
            message += "; temporary directory cleanup failed at " + temporaryPath + ": " + cleanupResult.message;
        }
        return {false, message};
    }

    const RemoteOperationResult cleanupResult = cleanupRemoteTree(remoteFileSystem, backupPath);
    if (!cleanupResult.success)
    {
        return {false, "remote directory replacement succeeded but backup cleanup failed at "
                + backupPath + ": " + cleanupResult.message};
    }
    return {true, "remote directory replaced safely"};
}

RemoteOperationResult downloadDirectoryReplacing(
    RemoteFileSystem &remoteFileSystem,
    const std::string &remoteDirectoryPath,
    const std::string &localDirectoryPath,
    TransferProgressCallback progress)
{
    const std::filesystem::path targetPath = std::filesystem::u8path(localDirectoryPath);
    std::error_code localError;
    const std::filesystem::file_status targetStatus = std::filesystem::symlink_status(targetPath, localError);
    if (localError || !std::filesystem::is_directory(targetStatus) || std::filesystem::is_symlink(targetStatus))
    {
        return {false, "local directory replacement target is not a regular directory: " + localDirectoryPath};
    }

    const std::string normalizedSource = normalizeRemotePath(remoteDirectoryPath);
    const std::string sourceName = remoteBaseName(normalizedSource);
    if (sourceName.empty())
    {
        return {false, "remote directory replacement source must not be the root directory"};
    }

    std::vector<FileItem> sourceSiblings;
    try
    {
        sourceSiblings = remoteFileSystem.listDirectory(remoteParentPath(normalizedSource));
    }
    catch (const std::exception &error)
    {
        return {false, std::string("failed to inspect remote directory replacement source: ") + error.what()};
    }
    const auto source = std::find_if(sourceSiblings.begin(), sourceSiblings.end(), [&normalizedSource](const FileItem &item) {
        return normalizeRemotePath(item.path) == normalizedSource;
    });
    if (source == sourceSiblings.end() || source->type != FileItemType::Directory)
    {
        return {false, "remote directory replacement source is not a directory: " + normalizedSource};
    }

    struct RemoteEntry
    {
        std::string remotePath;
        std::filesystem::path relativePath;
        bool directory = false;
        std::int64_t size = -1;
    };

    std::vector<RemoteEntry> remoteEntries;
    std::set<std::string> visitedDirectories;
    std::int64_t totalBytes = 0;
    bool totalBytesKnown = true;
    std::string scanError;
    std::function<bool(const std::string &, const std::filesystem::path &)> collectDirectory;
    collectDirectory = [&](const std::string &remotePath, const std::filesystem::path &relativePath) {
        const std::string normalizedPath = normalizeRemotePath(remotePath);
        if (!visitedDirectories.insert(normalizedPath).second)
        {
            scanError = "remote directory replacement source contains a directory cycle at " + normalizedPath;
            return false;
        }

        std::vector<FileItem> children;
        try
        {
            children = remoteFileSystem.listDirectory(normalizedPath);
        }
        catch (const std::exception &error)
        {
            scanError = std::string("failed to list remote directory replacement source at ")
                + normalizedPath + ": " + error.what();
            return false;
        }

        for (const FileItem &child : children)
        {
            if (!isValidRemoteChildName(child.name))
            {
                scanError = "remote directory replacement source contains an invalid child name at "
                    + normalizedPath;
                return false;
            }
            const std::string expectedRemotePath = joinRemotePath(normalizedPath, child.name);
            if (normalizeRemotePath(child.path) != expectedRemotePath)
            {
                scanError = "remote directory replacement source contains an inconsistent child path: " + child.path;
                return false;
            }

            RemoteEntry entry;
            entry.remotePath = expectedRemotePath;
            entry.relativePath = relativePath / std::filesystem::u8path(child.name);
            entry.directory = child.type == FileItemType::Directory;
            entry.size = child.size;
            if (entry.directory)
            {
                remoteEntries.push_back(entry);
                if (!collectDirectory(expectedRemotePath, entry.relativePath))
                {
                    return false;
                }
                continue;
            }
            if (child.type != FileItemType::File && child.type != FileItemType::Other)
            {
                scanError = "unsupported remote directory entry at " + expectedRemotePath;
                return false;
            }
            if (entry.size < 0)
            {
                totalBytesKnown = false;
            }
            else if (entry.size > std::numeric_limits<std::int64_t>::max() - totalBytes)
            {
                scanError = "remote directory replacement source is too large to report progress: " + normalizedSource;
                return false;
            }
            else
            {
                totalBytes += entry.size;
            }
            remoteEntries.push_back(std::move(entry));
        }
        return true;
    };

    if (!collectDirectory(normalizedSource, {}))
    {
        return {false, scanError};
    }
    const std::int64_t reportedTotalBytes = totalBytesKnown ? totalBytes : -1;

    const std::filesystem::path parent = targetPath.has_parent_path()
        ? targetPath.parent_path()
        : std::filesystem::current_path();
    const std::filesystem::path temporaryPath = uniqueLocalArtifactPath(parent, "download-directory");
    const std::filesystem::path backupPath = uniqueLocalArtifactPath(parent, "backup-directory");
    if (temporaryPath.empty() || backupPath.empty() || temporaryPath == backupPath)
    {
        return {false, "failed to allocate unique local directory replacement paths"};
    }

    std::filesystem::create_directory(temporaryPath, localError);
    if (localError)
    {
        return {false, "failed to create temporary local directory at "
                + localPathText(temporaryPath) + ": " + localError.message()};
    }
    const auto failAndCleanTemporary = [&](const std::string &message) {
        const RemoteOperationResult cleanupResult = cleanupLocalTree(temporaryPath);
        return cleanupResult.success
            ? RemoteOperationResult{false, message}
            : RemoteOperationResult{false, message + "; temporary directory cleanup failed at "
                    + localPathText(temporaryPath) + ": " + cleanupResult.message};
    };

    std::int64_t completedBytes = 0;
    const auto notifyProgress = [&](std::int64_t transferredBytes) {
        return progress ? progress(transferredBytes, reportedTotalBytes) : true;
    };
    if (!notifyProgress(0))
    {
        return failAndCleanTemporary("local directory replacement was canceled before download");
    }

    for (const RemoteEntry &entry : remoteEntries)
    {
        if (!notifyProgress(completedBytes))
        {
            return failAndCleanTemporary("local directory replacement was canceled during download");
        }
        const std::filesystem::path localEntryPath = temporaryPath / entry.relativePath;
        if (entry.directory)
        {
            std::filesystem::create_directory(localEntryPath, localError);
            if (localError)
            {
                return failAndCleanTemporary("failed to create temporary local subdirectory at "
                    + localPathText(localEntryPath) + ": " + localError.message());
            }
            continue;
        }

        std::int64_t downloadedFileBytes = 0;
        const RemoteOperationResult downloadResult = remoteFileSystem.downloadFile(
            entry.remotePath,
            localPathText(localEntryPath),
            [&](std::int64_t fileBytes, std::int64_t) {
                downloadedFileBytes = std::max(downloadedFileBytes, std::max<std::int64_t>(0, fileBytes));
                return notifyProgress(completedBytes + downloadedFileBytes);
            });
        if (!downloadResult.success)
        {
            return failAndCleanTemporary("failed to download temporary local directory file at "
                + localPathText(localEntryPath) + ": " + downloadResult.message);
        }
        completedBytes += entry.size >= 0 ? entry.size : downloadedFileBytes;
    }
    if (!notifyProgress(totalBytesKnown ? totalBytes : completedBytes))
    {
        return failAndCleanTemporary("local directory replacement was canceled after download");
    }

    localError.clear();
    const std::filesystem::file_status currentTargetStatus = std::filesystem::symlink_status(targetPath, localError);
    if (localError || !std::filesystem::is_directory(currentTargetStatus)
        || std::filesystem::is_symlink(currentTargetStatus))
    {
        return failAndCleanTemporary("local directory replacement target changed before replacement: " + localDirectoryPath);
    }

    std::filesystem::rename(targetPath, backupPath, localError);
    if (localError)
    {
        return failAndCleanTemporary("failed to move original local directory to backup "
            + localPathText(backupPath) + ": " + localError.message());
    }

    localError.clear();
    std::filesystem::rename(temporaryPath, targetPath, localError);
    if (localError)
    {
        const std::string replaceError = localError.message();
        std::error_code restoreError;
        std::filesystem::rename(backupPath, targetPath, restoreError);
        const RemoteOperationResult cleanupResult = cleanupLocalTree(temporaryPath);
        std::string message = !restoreError
            ? "local directory replacement failed and original directory was restored from "
                + localPathText(backupPath) + ": " + replaceError
            : "local directory replacement failed and original directory could not be restored from "
                + localPathText(backupPath) + ": " + restoreError.message() + "; replacement error: " + replaceError;
        if (!cleanupResult.success)
        {
            message += "; temporary directory cleanup failed at "
                + localPathText(temporaryPath) + ": " + cleanupResult.message;
        }
        return {false, message};
    }

    const RemoteOperationResult cleanupResult = cleanupLocalTree(backupPath);
    if (!cleanupResult.success)
    {
        return {false, "local directory replacement succeeded but backup cleanup failed at "
                + localPathText(backupPath) + ": " + cleanupResult.message};
    }
    return {true, "local directory replaced safely"};
}

RemoteOperationResult downloadFileReplacing(
    RemoteFileSystem &remoteFileSystem,
    const std::string &remotePath,
    const std::string &localPath,
    TransferProgressCallback progress)
{
    const std::filesystem::path targetPath = std::filesystem::u8path(localPath);
    std::error_code error;
    const std::filesystem::file_status targetStatus = std::filesystem::symlink_status(targetPath, error);
    if (error || !std::filesystem::is_regular_file(targetStatus))
    {
        return {false, "local replacement target is not a regular file: " + localPath};
    }

    const std::filesystem::path parent = targetPath.has_parent_path()
        ? targetPath.parent_path()
        : std::filesystem::current_path();
    const std::filesystem::path temporaryPath = uniqueLocalArtifactPath(parent, "download");
    const std::filesystem::path backupPath = uniqueLocalArtifactPath(parent, "backup");
    if (temporaryPath.empty() || backupPath.empty() || temporaryPath == backupPath)
    {
        return {false, "failed to allocate unique local replacement paths"};
    }

    const RemoteOperationResult downloadResult = remoteFileSystem.downloadFile(
        remotePath,
        localPathText(temporaryPath),
        std::move(progress));
    if (!downloadResult.success)
    {
        const RemoteOperationResult cleanupResult = cleanupLocalFile(temporaryPath);
        std::string message = "temporary local download failed at " + localPathText(temporaryPath) + ": " + downloadResult.message;
        if (!cleanupResult.success)
        {
            message += "; temporary artifact cleanup failed at " + localPathText(temporaryPath) + ": " + cleanupResult.message;
        }
        return {false, message};
    }

    error.clear();
    const std::filesystem::file_status currentTargetStatus = std::filesystem::symlink_status(targetPath, error);
    if (error || !std::filesystem::is_regular_file(currentTargetStatus))
    {
        const RemoteOperationResult cleanupResult = cleanupLocalFile(temporaryPath);
        std::string message = "local replacement target changed before replacement: " + localPath;
        if (!cleanupResult.success)
        {
            message += "; temporary artifact cleanup failed at " + localPathText(temporaryPath) + ": " + cleanupResult.message;
        }
        return {false, message};
    }

    std::filesystem::rename(targetPath, backupPath, error);
    if (error)
    {
        const RemoteOperationResult cleanupResult = cleanupLocalFile(temporaryPath);
        std::string message = "failed to move original local target to backup " + localPathText(backupPath) + ": " + error.message();
        if (!cleanupResult.success)
        {
            message += "; temporary artifact cleanup failed at " + localPathText(temporaryPath) + ": " + cleanupResult.message;
        }
        return {false, message};
    }

    error.clear();
    std::filesystem::rename(temporaryPath, targetPath, error);
    if (error)
    {
        const std::string replaceError = error.message();
        std::error_code restoreError;
        std::filesystem::rename(backupPath, targetPath, restoreError);
        const RemoteOperationResult cleanupResult = cleanupLocalFile(temporaryPath);
        std::string message;
        if (!restoreError)
        {
            message = "local replacement failed and original target was restored from " + localPathText(backupPath)
                + ": " + replaceError;
        }
        else
        {
            message = "local replacement failed and original target could not be restored from " + localPathText(backupPath)
                + ": " + restoreError.message() + "; replacement error: " + replaceError;
        }
        if (!cleanupResult.success)
        {
            message += "; temporary artifact cleanup failed at " + localPathText(temporaryPath) + ": " + cleanupResult.message;
        }
        return {false, message};
    }

    const RemoteOperationResult cleanupResult = cleanupLocalFile(backupPath);
    if (!cleanupResult.success)
    {
        return {false, "local replacement succeeded but backup cleanup failed at " + localPathText(backupPath)
                + ": " + cleanupResult.message};
    }
    return {true, "local file replaced safely"};
}
}
