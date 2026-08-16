#include "config/SettingsStore.h"
#include "config/SiteProfile.h"
#include "config/SiteStore.h"
#include "config/PasswordCrypto.h"
#include "config/UserSettings.h"
#include "core/ExternalEditDocument.h"
#include "core/FakeRemoteFileSystem.h"
#include "core/FileCache.h"
#include "core/FileReplacement.h"
#include "core/TransferJob.h"
#include "core/TransferManager.h"
#include "core/TransferQueue.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace
{
void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

bool containsPath(const std::vector<FileItem> &items, const std::string &path, FileItemType type)
{
    for (const FileItem &item : items)
    {
        if (item.path == path && item.type == type)
        {
            return true;
        }
    }
    return false;
}

std::string permissionsForPath(const std::vector<FileItem> &items, const std::string &path)
{
    for (const FileItem &item : items)
    {
        if (item.path == path)
        {
            return item.permissions;
        }
    }
    return {};
}

template <typename Callable>
void requireThrows(Callable callable, const std::string &message)
{
    try
    {
        callable();
    }
    catch (const std::exception &)
    {
        return;
    }

    throw std::runtime_error(message);
}

bool isReplacementArtifactPath(const std::string &path)
{
    const std::size_t slashIndex = path.find_last_of('/');
    const std::string name = slashIndex == std::string::npos ? path : path.substr(slashIndex + 1);
    return name.rfind(".dirbridge-", 0) == 0;
}

bool hasReplacementArtifact(const std::vector<FileItem> &items)
{
    return std::any_of(items.begin(), items.end(), [](const FileItem &item) {
        return isReplacementArtifactPath(item.path);
    });
}

std::string replacementArtifactPath(const std::vector<FileItem> &items, const std::string &role)
{
    const std::string prefix = ".dirbridge-" + role + '-';
    for (const FileItem &item : items)
    {
        if (item.name.rfind(prefix, 0) == 0)
        {
            return item.path;
        }
    }
    return {};
}

std::int64_t fileSizeForPath(const std::vector<FileItem> &items, const std::string &path)
{
    for (const FileItem &item : items)
    {
        if (item.path == path && item.type == FileItemType::File)
        {
            return item.size;
        }
    }
    return -1;
}

bool hasLocalReplacementArtifact(const std::filesystem::path &directory)
{
    for (const auto &entry : std::filesystem::directory_iterator(directory))
    {
        if (entry.path().filename().string().rfind(".dirbridge-", 0) == 0)
        {
            return true;
        }
    }
    return false;
}

std::string readFileText(const std::filesystem::path &path)
{
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

class ReplacementFaultRemoteFileSystem : public FakeRemoteFileSystem
{
public:
    bool failTemporaryUpload = false;
    bool failTemporaryDownload = false;
    bool failBackupRename = false;
    bool failReplaceRename = false;
    bool failRestoreRename = false;
    bool failCleanup = false;
    int uploadCalls = 0;
    int downloadCalls = 0;
    int renameCalls = 0;
    std::string lastUploadPath;
    std::string lastDownloadPath;

    RemoteOperationResult uploadFile(
        const std::string &localPath,
        const std::string &remotePath,
        TransferProgressCallback progress = {}) override
    {
        ++uploadCalls;
        lastUploadPath = remotePath;
        if (failTemporaryUpload && remotePath.find("/.dirbridge-upload-") != std::string::npos)
        {
            const RemoteOperationResult uploadResult = FakeRemoteFileSystem::uploadFile(
                localPath,
                remotePath,
                std::move(progress));
            return uploadResult.success
                ? RemoteOperationResult{false, "injected temporary upload failure after write"}
                : uploadResult;
        }
        return FakeRemoteFileSystem::uploadFile(localPath, remotePath, std::move(progress));
    }

    RemoteOperationResult downloadFile(
        const std::string &remotePath,
        const std::string &localPath,
        TransferProgressCallback progress = {}) override
    {
        ++downloadCalls;
        lastDownloadPath = localPath;
        const RemoteOperationResult downloadResult = FakeRemoteFileSystem::downloadFile(
            remotePath,
            localPath,
            std::move(progress));
        if (failTemporaryDownload
            && localPath.find(".dirbridge-download-directory-") != std::string::npos
            && downloadResult.success)
        {
            return {false, "injected temporary directory download failure after write"};
        }
        return downloadResult;
    }

    RemoteOperationResult rename(const std::string &sourcePath, const std::string &targetPath) override
    {
        ++renameCalls;
        if (failBackupRename && targetPath.find("/.dirbridge-backup-") != std::string::npos)
        {
            return {false, "injected backup rename failure"};
        }
        if (failReplaceRename && sourcePath.find("/.dirbridge-upload-") != std::string::npos)
        {
            return {false, "injected replacement rename failure"};
        }
        if (failRestoreRename && sourcePath.find("/.dirbridge-backup-") != std::string::npos)
        {
            return {false, "injected restore rename failure"};
        }
        return FakeRemoteFileSystem::rename(sourcePath, targetPath);
    }

    RemoteOperationResult remove(const std::string &path) override
    {
        if (failCleanup && isReplacementArtifactPath(path))
        {
            return {false, "injected artifact cleanup failure"};
        }
        return FakeRemoteFileSystem::remove(path);
    }
};

void checkFakeRemoteFileSystem()
{
    SiteProfile profile;
    profile.name = "fake";
    profile.protocol = RemoteProtocol::Sftp;
    profile.host = "fake-host";
    profile.port = 22;
    profile.username = "testuser";
    profile.defaultRemotePath = "/home/testuser/remote_test";

    FakeRemoteFileSystem remote;
    RemoteOperationResult result = remote.connect(profile);
    require(result.success, "fake remote should connect with a host");
    require(remote.isConnected(), "fake remote should report connected");

    std::vector<FileItem> items = remote.listDirectory(profile.defaultRemotePath);
    require(containsPath(items, "/home/testuser/remote_test/download", FileItemType::Directory), "initial download directory missing");
    require(containsPath(items, "/home/testuser/remote_test/upload", FileItemType::Directory), "initial upload directory missing");
    require(containsPath(items, "/home/testuser/remote_test/edit", FileItemType::Directory), "initial edit directory missing");

    result = remote.createDirectory("/home/testuser/remote_test/new_folder");
    require(result.success, "create directory should succeed");
    items = remote.listDirectory(profile.defaultRemotePath);
    require(containsPath(items, "/home/testuser/remote_test/new_folder", FileItemType::Directory), "created directory not listed");

    result = remote.createDirectory("/home/testuser/remote_test/new_folder");
    require(!result.success, "duplicate directory create should fail");

    result = remote.createDirectory("/home/testuser/missing_parent/new_folder");
    require(!result.success, "create under missing parent should fail");

    result = remote.createFile("/home/testuser/remote_test/empty.txt");
    require(result.success, "create file should succeed");
    items = remote.listDirectory(profile.defaultRemotePath);
    require(containsPath(items, "/home/testuser/remote_test/empty.txt", FileItemType::File), "created file not listed");

    result = remote.setPermissions("/home/testuser/remote_test/empty.txt", 0750);
    require(result.success, "set permissions should succeed");
    items = remote.listDirectory(profile.defaultRemotePath);
    require(permissionsForPath(items, "/home/testuser/remote_test/empty.txt") == "-rwxr-x---", "updated file permissions not listed");
    result = remote.setPermissions("/home/testuser/remote_test/empty.txt", 01000);
    require(!result.success, "permission mode above 0777 should fail");
    result = remote.setPermissions("/home/testuser/remote_test/missing.txt", 0644);
    require(!result.success, "set permissions on missing item should fail");

    result = remote.createFile("/home/testuser/remote_test/empty.txt");
    require(!result.success, "duplicate file create should fail");

    result = remote.createFile("/home/testuser/missing_parent/empty.txt");
    require(!result.success, "create file under missing parent should fail");

    result = remote.rename("/home/testuser/remote_test/new_folder", "/home/testuser/remote_test/renamed_folder");
    require(result.success, "rename directory should succeed");
    items = remote.listDirectory(profile.defaultRemotePath);
    require(containsPath(items, "/home/testuser/remote_test/renamed_folder", FileItemType::Directory), "renamed directory not listed");

    result = remote.rename("/home/testuser/remote_test/renamed_folder", "/home/testuser/remote_test/upload");
    require(!result.success, "rename over existing target should fail");

    result = remote.createFile("/home/testuser/remote_test/renamed_folder/nested.txt");
    require(result.success, "create file under renamed directory should succeed");

    result = remote.rename("/home/testuser/remote_test/renamed_folder", "/home/testuser/renamed_folder");
    require(result.success, "cross-directory rename should succeed in fake backend");
    items = remote.listDirectory("/home/testuser");
    require(containsPath(items, "/home/testuser/renamed_folder", FileItemType::Directory), "moved directory not listed under target parent");
    items = remote.listDirectory("/home/testuser/renamed_folder");
    require(containsPath(items, "/home/testuser/renamed_folder/nested.txt", FileItemType::File), "moved directory child path not updated");

    result = remote.rename("/home/testuser/renamed_folder", "/home/testuser/remote_test/renamed_folder");
    require(result.success, "move directory back into remote test should succeed");

    result = remote.createDirectory("/home/testuser/remote_test/upload/nested");
    require(result.success, "create nested directory should succeed");

    result = remote.remove("/home/testuser/remote_test/upload");
    require(!result.success, "remove non-empty directory should fail");

    result = remote.remove("/home/testuser/remote_test/renamed_folder/nested.txt");
    require(result.success, "remove nested file before empty directory delete should succeed");

    result = remote.remove("/home/testuser/remote_test/renamed_folder");
    require(result.success, "remove empty directory should succeed");
    items = remote.listDirectory(profile.defaultRemotePath);
    require(!containsPath(items, "/home/testuser/remote_test/renamed_folder", FileItemType::Directory), "removed directory still listed");

    result = remote.remove("/home/testuser/remote_test/missing_folder");
    require(!result.success, "remove missing item should fail");

    result = remote.remove("/");
    require(!result.success, "remove root should fail");

    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "dirbridge-core-checks";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot);
    const std::filesystem::path uploadSource = tempRoot / "upload.txt";
    {
        std::ofstream output(uploadSource, std::ios::binary | std::ios::trunc);
        output << "hello dirbridge\n";
    }

    result = remote.uploadFile(uploadSource.string(), "/home/testuser/remote_test/upload.txt");
    require(result.success, "upload file should succeed");
    items = remote.listDirectory(profile.defaultRemotePath);
    require(containsPath(items, "/home/testuser/remote_test/upload.txt", FileItemType::File), "uploaded file not listed");

    result = remote.uploadFile((tempRoot / "missing-upload.txt").string(), "/home/testuser/remote_test/missing-upload.txt");
    require(!result.success, "upload missing local file should fail");

    result = remote.uploadFile(uploadSource.string(), "/home/testuser/missing_parent/upload.txt");
    require(!result.success, "upload to missing remote parent should fail");

    const std::filesystem::path downloadTarget = tempRoot / "download.txt";
    result = remote.downloadFile("/home/testuser/remote_test/upload.txt", downloadTarget.string());
    require(result.success, "download file should succeed: " + result.message);
    require(std::filesystem::is_regular_file(downloadTarget), "download target should exist");

    result = remote.downloadFile("/home/testuser/remote_test/missing-download.txt", (tempRoot / "missing-download.txt").string());
    require(!result.success, "download missing remote file should fail");

    result = remote.downloadFile("/home/testuser/remote_test/upload", (tempRoot / "directory-download.txt").string());
    require(!result.success, "download remote directory should fail");

    remote.disconnect();
    require(!remote.isConnected(), "fake remote should report disconnected");
    requireThrows([&remote, &profile]() { remote.listDirectory(profile.defaultRemotePath); }, "list after disconnect should throw");
    result = remote.createDirectory("/home/testuser/remote_test/after_disconnect");
    require(!result.success, "create after disconnect should fail");
    result = remote.createFile("/home/testuser/remote_test/after_disconnect.txt");
    require(!result.success, "create file after disconnect should fail");
    result = remote.remove("/home/testuser/remote_test/upload.txt");
    require(!result.success, "remove after disconnect should fail");
    result = remote.rename("/home/testuser/remote_test/upload.txt", "/home/testuser/remote_test/renamed.txt");
    require(!result.success, "rename after disconnect should fail");
    result = remote.uploadFile(uploadSource.string(), "/home/testuser/remote_test/after_disconnect.txt");
    require(!result.success, "upload after disconnect should fail");
    result = remote.downloadFile("/home/testuser/remote_test/upload.txt", (tempRoot / "after-disconnect.txt").string());
    require(!result.success, "download after disconnect should fail");
}

void checkFileReplacement()
{
    SiteProfile profile;
    profile.name = "replacement-fake";
    profile.protocol = RemoteProtocol::Sftp;
    profile.host = "fake-host";
    profile.port = 22;
    profile.username = "testuser";
    profile.defaultRemotePath = "/home/testuser/remote_test";

    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "dirbridge-file-replacement-checks";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot);
    const std::filesystem::path originalSource = tempRoot / "original.txt";
    const std::filesystem::path replacementSource = tempRoot / "replacement.txt";
    {
        std::ofstream output(originalSource, std::ios::binary | std::ios::trunc);
        output << "old";
    }
    {
        std::ofstream output(replacementSource, std::ios::binary | std::ios::trunc);
        output << "new replacement content";
    }
    const auto originalSize = static_cast<std::int64_t>(std::filesystem::file_size(originalSource));
    const auto replacementSize = static_cast<std::int64_t>(std::filesystem::file_size(replacementSource));
    const std::string remoteTarget = "/home/testuser/remote_test/replaced.txt";

    const auto seedTarget = [&](ReplacementFaultRemoteFileSystem &remote) {
        RemoteOperationResult result = remote.connect(profile);
        require(result.success, "replacement fake remote should connect");
        result = remote.uploadFile(originalSource.string(), remoteTarget);
        require(result.success, "replacement target seed upload should succeed");
        remote.uploadCalls = 0;
        remote.renameCalls = 0;
        remote.lastUploadPath.clear();
    };

    ReplacementFaultRemoteFileSystem successfulRemote;
    seedTarget(successfulRemote);
    RemoteOperationResult result = file_replacement::uploadFileReplacing(
        successfulRemote,
        replacementSource.string(),
        remoteTarget);
    require(result.success, "remote safe replacement should succeed: " + result.message);
    std::vector<FileItem> items = successfulRemote.listDirectory(profile.defaultRemotePath);
    require(fileSizeForPath(items, remoteTarget) == replacementSize, "remote safe replacement should install new file");
    require(!hasReplacementArtifact(items), "successful remote replacement should clean all artifacts");
    require(successfulRemote.uploadCalls == 1
            && successfulRemote.lastUploadPath.find("/.dirbridge-upload-") != std::string::npos,
        "remote replacement should upload exactly one temporary file");
    require(successfulRemote.renameCalls == 2, "remote replacement should back up and then install the target");

    result = successfulRemote.uploadFile(originalSource.string(), remoteTarget);
    require(result.success, "reset replacement target should succeed");
    result = file_replacement::uploadFileReplacing(
        successfulRemote,
        replacementSource.string(),
        remoteTarget,
        [](std::int64_t, std::int64_t) { return false; });
    require(!result.success, "canceled remote replacement should fail");
    items = successfulRemote.listDirectory(profile.defaultRemotePath);
    require(fileSizeForPath(items, remoteTarget) == originalSize, "canceled remote replacement should preserve original target");
    require(!hasReplacementArtifact(items), "canceled remote replacement should clean temporary artifacts");

    ReplacementFaultRemoteFileSystem temporaryUploadFailure;
    seedTarget(temporaryUploadFailure);
    temporaryUploadFailure.failTemporaryUpload = true;
    result = file_replacement::uploadFileReplacing(temporaryUploadFailure, replacementSource.string(), remoteTarget);
    require(!result.success, "injected temporary upload failure should fail replacement");
    items = temporaryUploadFailure.listDirectory(profile.defaultRemotePath);
    require(fileSizeForPath(items, remoteTarget) == originalSize, "temporary upload failure should preserve original target");
    require(!hasReplacementArtifact(items), "temporary upload failure should clean the partially written artifact");

    ReplacementFaultRemoteFileSystem temporaryCleanupFailure;
    seedTarget(temporaryCleanupFailure);
    temporaryCleanupFailure.failTemporaryUpload = true;
    temporaryCleanupFailure.failCleanup = true;
    result = file_replacement::uploadFileReplacing(temporaryCleanupFailure, replacementSource.string(), remoteTarget);
    require(!result.success, "temporary artifact cleanup failure should be reported");
    items = temporaryCleanupFailure.listDirectory(profile.defaultRemotePath);
    const std::string retainedTemporary = replacementArtifactPath(items, "upload");
    require(fileSizeForPath(items, remoteTarget) == originalSize,
        "temporary cleanup failure should preserve original target");
    require(!retainedTemporary.empty() && result.message.find(retainedTemporary) != std::string::npos,
        "temporary cleanup failure should report the retained temporary path");

    ReplacementFaultRemoteFileSystem backupFailure;
    seedTarget(backupFailure);
    backupFailure.failBackupRename = true;
    result = file_replacement::uploadFileReplacing(backupFailure, replacementSource.string(), remoteTarget);
    require(!result.success, "injected backup failure should fail replacement");
    items = backupFailure.listDirectory(profile.defaultRemotePath);
    require(fileSizeForPath(items, remoteTarget) == originalSize, "backup failure should preserve original target");
    require(!hasReplacementArtifact(items), "backup failure should clean temporary upload");

    ReplacementFaultRemoteFileSystem replaceFailure;
    seedTarget(replaceFailure);
    replaceFailure.failReplaceRename = true;
    result = file_replacement::uploadFileReplacing(replaceFailure, replacementSource.string(), remoteTarget);
    require(!result.success && result.message.find("was restored") != std::string::npos,
        "replacement failure should report successful rollback");
    items = replaceFailure.listDirectory(profile.defaultRemotePath);
    require(fileSizeForPath(items, remoteTarget) == originalSize, "replacement failure should restore original target");
    require(!hasReplacementArtifact(items), "successful rollback should clean replacement artifacts");

    ReplacementFaultRemoteFileSystem restoreFailure;
    seedTarget(restoreFailure);
    restoreFailure.failReplaceRename = true;
    restoreFailure.failRestoreRename = true;
    result = file_replacement::uploadFileReplacing(restoreFailure, replacementSource.string(), remoteTarget);
    require(!result.success, "injected rollback failure should fail replacement");
    items = restoreFailure.listDirectory(profile.defaultRemotePath);
    const std::string retainedBackup = replacementArtifactPath(items, "backup");
    require(!retainedBackup.empty(), "rollback failure should retain the original backup");
    require(result.message.find(retainedBackup) != std::string::npos,
        "rollback failure should report the retained backup path");

    ReplacementFaultRemoteFileSystem cleanupFailure;
    seedTarget(cleanupFailure);
    cleanupFailure.failCleanup = true;
    result = file_replacement::uploadFileReplacing(cleanupFailure, replacementSource.string(), remoteTarget);
    require(!result.success, "backup cleanup failure should be reported");
    items = cleanupFailure.listDirectory(profile.defaultRemotePath);
    const std::string retainedCleanupBackup = replacementArtifactPath(items, "backup");
    require(fileSizeForPath(items, remoteTarget) == replacementSize,
        "cleanup failure should not undo the installed replacement");
    require(!retainedCleanupBackup.empty() && result.message.find(retainedCleanupBackup) != std::string::npos,
        "cleanup failure should report the retained backup path");

    ReplacementFaultRemoteFileSystem invalidTargetRemote;
    seedTarget(invalidTargetRemote);
    result = invalidTargetRemote.createDirectory("/home/testuser/remote_test/replacement-directory");
    require(result.success, "replacement directory fixture should be created");
    result = file_replacement::uploadFileReplacing(
        invalidTargetRemote,
        replacementSource.string(),
        "/home/testuser/remote_test/replacement-directory");
    require(!result.success, "remote replacement should reject a directory target");
    result = file_replacement::uploadFileReplacing(
        invalidTargetRemote,
        replacementSource.string(),
        "/home/testuser/remote_test/missing-replacement.txt");
    require(!result.success, "remote replacement should reject a missing target");
    items = invalidTargetRemote.listDirectory(profile.defaultRemotePath);
    require(containsPath(items, "/home/testuser/remote_test/replacement-directory", FileItemType::Directory),
        "rejected directory replacement should preserve the directory");
    require(!hasReplacementArtifact(items), "rejected remote replacement targets should not create artifacts");

    ReplacementFaultRemoteFileSystem ordinaryTransferRemote;
    seedTarget(ordinaryTransferRemote);
    TransferQueue ordinaryQueue;
    TransferJob ordinaryUpload;
    ordinaryUpload.id = "ordinary-upload";
    ordinaryUpload.name = "ordinary.txt";
    ordinaryUpload.direction = TransferDirection::Upload;
    ordinaryUpload.localPath = replacementSource.string();
    ordinaryUpload.remotePath = "/home/testuser/remote_test/ordinary.txt";
    ordinaryQueue.enqueue(ordinaryUpload);
    TransferManager ordinaryManager(ordinaryTransferRemote, ordinaryQueue);
    ordinaryManager.processPending();
    require(ordinaryQueue.find(ordinaryUpload.id)->status == TransferStatus::Completed,
        "ordinary upload should complete");
    require(ordinaryTransferRemote.lastUploadPath == ordinaryUpload.remotePath
            && ordinaryTransferRemote.renameCalls == 0,
        "ordinary upload should write directly without replacement artifacts");

    result = ordinaryTransferRemote.uploadFile(originalSource.string(), remoteTarget);
    require(result.success, "reset manager replacement target should succeed");
    ordinaryTransferRemote.uploadCalls = 0;
    ordinaryTransferRemote.renameCalls = 0;
    ordinaryTransferRemote.lastUploadPath.clear();
    TransferQueue replacementQueue;
    TransferJob replacementUpload = ordinaryUpload;
    replacementUpload.id = "replacement-upload";
    replacementUpload.remotePath = remoteTarget;
    replacementUpload.replaceExisting = true;
    replacementQueue.enqueue(replacementUpload);
    TransferManager replacementManager(ordinaryTransferRemote, replacementQueue);
    replacementManager.processPending();
    require(replacementQueue.find(replacementUpload.id)->status == TransferStatus::Completed,
        "replacement upload task should complete");
    require(ordinaryTransferRemote.lastUploadPath.find("/.dirbridge-upload-") != std::string::npos
            && ordinaryTransferRemote.renameCalls == 2,
        "replacement upload task should use the safe replacement path");

    FakeRemoteFileSystem downloadRemote;
    result = downloadRemote.connect(profile);
    require(result.success, "download replacement fake remote should connect");
    const std::filesystem::path localTarget = tempRoot / "download-target.txt";
    {
        std::ofstream output(localTarget, std::ios::binary | std::ios::trunc);
        output << "preserve local original";
    }
    result = file_replacement::downloadFileReplacing(
        downloadRemote,
        "/home/testuser/remote_test/readme.txt",
        localTarget.string());
    require(result.success, "local download replacement should succeed: " + result.message);
    require(readFileText(localTarget).find("fake remote file:") == 0,
        "local download replacement should install downloaded content");
    require(!hasLocalReplacementArtifact(tempRoot), "successful local replacement should clean all artifacts");

    {
        std::ofstream output(localTarget, std::ios::binary | std::ios::trunc);
        output << "preserve canceled local original";
    }
    result = file_replacement::downloadFileReplacing(
        downloadRemote,
        "/home/testuser/remote_test/readme.txt",
        localTarget.string(),
        [](std::int64_t, std::int64_t) { return false; });
    require(!result.success, "canceled local replacement should fail");
    require(readFileText(localTarget) == "preserve canceled local original",
        "canceled local replacement should preserve original content");
    require(!hasLocalReplacementArtifact(tempRoot), "canceled local replacement should clean temporary file");

    const std::filesystem::path missingLocalTarget = tempRoot / "missing-target.txt";
    result = file_replacement::downloadFileReplacing(
        downloadRemote,
        "/home/testuser/remote_test/readme.txt",
        missingLocalTarget.string());
    require(!result.success && !std::filesystem::exists(missingLocalTarget),
        "local replacement should reject a missing target");

    const std::filesystem::path directoryTarget = tempRoot / "directory-target";
    std::filesystem::create_directories(directoryTarget);
    result = file_replacement::downloadFileReplacing(
        downloadRemote,
        "/home/testuser/remote_test/readme.txt",
        directoryTarget.string());
    require(!result.success && std::filesystem::is_directory(directoryTarget),
        "local replacement should reject a directory target");

    ReplacementFaultRemoteFileSystem managerDownloadRemote;
    result = managerDownloadRemote.connect(profile);
    require(result.success, "manager download fake remote should connect");
    const std::filesystem::path ordinaryDownloadTarget = tempRoot / "ordinary-download.txt";
    TransferQueue ordinaryDownloadQueue;
    TransferJob ordinaryDownload;
    ordinaryDownload.id = "ordinary-download";
    ordinaryDownload.name = "ordinary-download.txt";
    ordinaryDownload.direction = TransferDirection::Download;
    ordinaryDownload.remotePath = "/home/testuser/remote_test/readme.txt";
    ordinaryDownload.localPath = ordinaryDownloadTarget.string();
    ordinaryDownloadQueue.enqueue(ordinaryDownload);
    TransferManager ordinaryDownloadManager(managerDownloadRemote, ordinaryDownloadQueue);
    ordinaryDownloadManager.processPending();
    require(ordinaryDownloadQueue.find(ordinaryDownload.id)->status == TransferStatus::Completed,
        "ordinary download should complete");
    require(managerDownloadRemote.downloadCalls == 1
            && managerDownloadRemote.lastDownloadPath == ordinaryDownload.localPath,
        "ordinary download should write directly to its target");

    const std::filesystem::path replacementDownloadTarget = tempRoot / "replacement-download.txt";
    {
        std::ofstream output(replacementDownloadTarget, std::ios::binary | std::ios::trunc);
        output << "replace this local file";
    }
    managerDownloadRemote.downloadCalls = 0;
    managerDownloadRemote.lastDownloadPath.clear();
    TransferQueue replacementDownloadQueue;
    TransferJob replacementDownload = ordinaryDownload;
    replacementDownload.id = "replacement-download";
    replacementDownload.localPath = replacementDownloadTarget.string();
    replacementDownload.replaceExisting = true;
    replacementDownloadQueue.enqueue(replacementDownload);
    TransferManager replacementDownloadManager(managerDownloadRemote, replacementDownloadQueue);
    replacementDownloadManager.processPending();
    require(replacementDownloadQueue.find(replacementDownload.id)->status == TransferStatus::Completed,
        "replacement download task should complete");
    require(managerDownloadRemote.downloadCalls == 1
            && managerDownloadRemote.lastDownloadPath.find(".dirbridge-download-") != std::string::npos
            && managerDownloadRemote.lastDownloadPath != replacementDownload.localPath,
        "replacement download task should use a temporary local path");
    require(readFileText(replacementDownloadTarget).find("fake remote file:") == 0,
        "replacement download task should install downloaded content");
    require(!hasLocalReplacementArtifact(tempRoot),
        "manager download checks should leave no local replacement artifacts");
}

void checkDirectoryReplacement()
{
    SiteProfile profile;
    profile.name = "directory-replacement-fake";
    profile.protocol = RemoteProtocol::Sftp;
    profile.host = "fake-host";
    profile.port = 22;
    profile.username = "testuser";
    profile.defaultRemotePath = "/home/testuser/remote_test";

    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path()
        / "dirbridge-directory-replacement-checks";
    std::filesystem::remove_all(tempRoot);
    const std::filesystem::path sourceRoot = tempRoot / "replacement-folder";
    const std::filesystem::path nestedRoot = sourceRoot / "nested";
    std::filesystem::create_directories(nestedRoot / "empty-child");
    {
        std::ofstream output(sourceRoot / "new-root.txt", std::ios::binary | std::ios::trunc);
        output << "new root content";
    }
    {
        std::ofstream output(nestedRoot / "new-nested.txt", std::ios::binary | std::ios::trunc);
        output << "new nested content";
    }

    const std::string remoteTarget = "/home/testuser/remote_test/replacement-folder";
    const std::string oldOnlyPath = remoteTarget + "/old-only.txt";
    const std::string newRootPath = remoteTarget + "/new-root.txt";
    const std::string newNestedDirectory = remoteTarget + "/nested";
    const std::string newNestedPath = newNestedDirectory + "/new-nested.txt";
    const std::string emptyDirectoryPath = newNestedDirectory + "/empty-child";

    const auto seedTarget = [&](ReplacementFaultRemoteFileSystem &remote) {
        RemoteOperationResult result = remote.connect(profile);
        require(result.success, "directory replacement fake remote should connect");
        result = remote.createDirectory(remoteTarget);
        require(result.success, "directory replacement target should be created");
        result = remote.createFile(oldOnlyPath);
        require(result.success, "directory replacement old-only file should be created");
        remote.uploadCalls = 0;
        remote.downloadCalls = 0;
        remote.renameCalls = 0;
        remote.lastUploadPath.clear();
        remote.lastDownloadPath.clear();
    };

    ReplacementFaultRemoteFileSystem successfulRemote;
    seedTarget(successfulRemote);
    int pendingEntryEvents = 0;
    int runningEntryEvents = 0;
    int completedEntryEvents = 0;
    int failedEntryEvents = 0;
    RemoteOperationResult result = file_replacement::uploadDirectoryReplacing(
        successfulRemote,
        sourceRoot.string(),
        remoteTarget,
        {},
        [&](const std::string &, std::int64_t, std::int64_t, file_replacement::DirectoryEntryTransferState state) {
            pendingEntryEvents += state == file_replacement::DirectoryEntryTransferState::Pending ? 1 : 0;
            runningEntryEvents += state == file_replacement::DirectoryEntryTransferState::Running ? 1 : 0;
            completedEntryEvents += state == file_replacement::DirectoryEntryTransferState::Completed ? 1 : 0;
            failedEntryEvents += state == file_replacement::DirectoryEntryTransferState::Failed ? 1 : 0;
        });
    require(result.success, "remote directory safe replacement should succeed: " + result.message);
    std::vector<FileItem> items = successfulRemote.listDirectory(profile.defaultRemotePath);
    require(!hasReplacementArtifact(items), "successful directory replacement should clean all artifacts");
    items = successfulRemote.listDirectory(remoteTarget);
    require(!containsPath(items, oldOnlyPath, FileItemType::File),
        "directory replacement should remove target-only content");
    require(containsPath(items, newRootPath, FileItemType::File)
            && containsPath(items, newNestedDirectory, FileItemType::Directory),
        "directory replacement should install root files and nested directories");
    items = successfulRemote.listDirectory(newNestedDirectory);
    require(containsPath(items, newNestedPath, FileItemType::File)
            && containsPath(items, emptyDirectoryPath, FileItemType::Directory),
        "directory replacement should preserve nested files and empty directories");
    require(successfulRemote.uploadCalls == 2 && successfulRemote.renameCalls == 2,
        "directory replacement should upload every file then perform two renames");
    require(pendingEntryEvents == 2 && runningEntryEvents >= 2
            && completedEntryEvents == 2 && failedEntryEvents == 0,
        "directory replacement should report each file as pending, running, and completed");

    ReplacementFaultRemoteFileSystem canceledRemote;
    seedTarget(canceledRemote);
    result = file_replacement::uploadDirectoryReplacing(
        canceledRemote,
        sourceRoot.string(),
        remoteTarget,
        [](std::int64_t, std::int64_t) { return false; });
    require(!result.success, "canceled directory replacement should fail");
    items = canceledRemote.listDirectory(remoteTarget);
    require(containsPath(items, oldOnlyPath, FileItemType::File),
        "canceled directory replacement should preserve the original directory");
    require(!hasReplacementArtifact(canceledRemote.listDirectory(profile.defaultRemotePath)),
        "canceled directory replacement should clean the temporary directory");

    ReplacementFaultRemoteFileSystem uploadFailure;
    seedTarget(uploadFailure);
    uploadFailure.failTemporaryUpload = true;
    result = file_replacement::uploadDirectoryReplacing(uploadFailure, sourceRoot.string(), remoteTarget);
    require(!result.success, "directory temporary upload failure should fail replacement");
    items = uploadFailure.listDirectory(remoteTarget);
    require(containsPath(items, oldOnlyPath, FileItemType::File),
        "directory upload failure should preserve the original directory");
    require(!hasReplacementArtifact(uploadFailure.listDirectory(profile.defaultRemotePath)),
        "directory upload failure should recursively clean the temporary directory");

    ReplacementFaultRemoteFileSystem backupFailure;
    seedTarget(backupFailure);
    backupFailure.failBackupRename = true;
    result = file_replacement::uploadDirectoryReplacing(backupFailure, sourceRoot.string(), remoteTarget);
    require(!result.success, "directory backup rename failure should fail replacement");
    require(containsPath(backupFailure.listDirectory(remoteTarget), oldOnlyPath, FileItemType::File),
        "directory backup failure should preserve the original directory");
    require(!hasReplacementArtifact(backupFailure.listDirectory(profile.defaultRemotePath)),
        "directory backup failure should clean the uploaded temporary directory");

    ReplacementFaultRemoteFileSystem replaceFailure;
    seedTarget(replaceFailure);
    replaceFailure.failReplaceRename = true;
    result = file_replacement::uploadDirectoryReplacing(replaceFailure, sourceRoot.string(), remoteTarget);
    require(!result.success && result.message.find("was restored") != std::string::npos,
        "directory replacement failure should report a successful rollback");
    require(containsPath(replaceFailure.listDirectory(remoteTarget), oldOnlyPath, FileItemType::File),
        "directory replacement failure should restore original contents");
    require(!hasReplacementArtifact(replaceFailure.listDirectory(profile.defaultRemotePath)),
        "successful directory rollback should clean all artifacts");

    ReplacementFaultRemoteFileSystem restoreFailure;
    seedTarget(restoreFailure);
    restoreFailure.failReplaceRename = true;
    restoreFailure.failRestoreRename = true;
    result = file_replacement::uploadDirectoryReplacing(restoreFailure, sourceRoot.string(), remoteTarget);
    require(!result.success, "directory rollback failure should fail replacement");
    items = restoreFailure.listDirectory(profile.defaultRemotePath);
    const std::string retainedBackup = replacementArtifactPath(items, "backup-directory");
    require(!retainedBackup.empty() && result.message.find(retainedBackup) != std::string::npos,
        "directory rollback failure should retain and report the original backup directory");
    require(containsPath(restoreFailure.listDirectory(retainedBackup), retainedBackup + "/old-only.txt", FileItemType::File),
        "retained directory backup should still contain original data");

    ReplacementFaultRemoteFileSystem cleanupFailure;
    seedTarget(cleanupFailure);
    cleanupFailure.failCleanup = true;
    result = file_replacement::uploadDirectoryReplacing(cleanupFailure, sourceRoot.string(), remoteTarget);
    require(!result.success, "directory backup cleanup failure should be reported");
    items = cleanupFailure.listDirectory(profile.defaultRemotePath);
    const std::string retainedCleanupBackup = replacementArtifactPath(items, "backup-directory");
    require(!retainedCleanupBackup.empty() && result.message.find(retainedCleanupBackup) != std::string::npos,
        "directory cleanup failure should report the retained backup directory path");
    require(containsPath(cleanupFailure.listDirectory(remoteTarget), newRootPath, FileItemType::File),
        "directory cleanup failure should not undo the installed replacement");

    ReplacementFaultRemoteFileSystem managerRemote;
    seedTarget(managerRemote);
    TransferQueue replacementQueue;
    TransferJob replacementJob;
    replacementJob.id = "directory-replacement";
    replacementJob.name = "replacement-folder";
    replacementJob.kind = TransferJobKind::DirectoryReplacement;
    replacementJob.direction = TransferDirection::Upload;
    replacementJob.localPath = sourceRoot.string();
    replacementJob.remotePath = remoteTarget;
    replacementJob.replaceExisting = true;
    replacementQueue.enqueue(replacementJob);
    TransferManager replacementManager(managerRemote, replacementQueue);
    replacementManager.processPending();
    require(replacementQueue.find(replacementJob.id)->status == TransferStatus::Completed,
        "directory replacement task should be runnable through TransferManager");
    require(containsPath(managerRemote.listDirectory(remoteTarget), newRootPath, FileItemType::File),
        "directory replacement task should install the new directory contents");

    const auto seedDownloadSource = [&](ReplacementFaultRemoteFileSystem &remote) {
        seedTarget(remote);
        const RemoteOperationResult uploadResult = file_replacement::uploadDirectoryReplacing(
            remote,
            sourceRoot.string(),
            remoteTarget);
        require(uploadResult.success, "directory replacement download source should be prepared");
        remote.uploadCalls = 0;
        remote.downloadCalls = 0;
        remote.renameCalls = 0;
        remote.lastUploadPath.clear();
        remote.lastDownloadPath.clear();
    };
    const auto seedLocalDownloadTarget = [&](const std::filesystem::path &target) {
        std::filesystem::remove_all(target);
        std::filesystem::create_directories(target);
        std::ofstream output(target / "old-only.txt", std::ios::binary | std::ios::trunc);
        output << "preserve old local directory";
    };

    const std::filesystem::path localDownloadTarget = tempRoot / "download-target";
    ReplacementFaultRemoteFileSystem successfulDownloadRemote;
    seedDownloadSource(successfulDownloadRemote);
    seedLocalDownloadTarget(localDownloadTarget);
    result = file_replacement::downloadDirectoryReplacing(
        successfulDownloadRemote,
        remoteTarget,
        localDownloadTarget.string());
    require(result.success, "local directory safe replacement should succeed: " + result.message);
    require(!std::filesystem::exists(localDownloadTarget / "old-only.txt"),
        "local directory replacement should remove target-only content");
    require(std::filesystem::is_regular_file(localDownloadTarget / "new-root.txt")
            && std::filesystem::is_regular_file(localDownloadTarget / "nested" / "new-nested.txt")
            && std::filesystem::is_directory(localDownloadTarget / "nested" / "empty-child"),
        "local directory replacement should install files, nested directories, and empty directories");
    require(successfulDownloadRemote.downloadCalls == 2,
        "local directory replacement should download every remote file");
    require(!hasLocalReplacementArtifact(tempRoot),
        "successful local directory replacement should clean all artifacts");

    ReplacementFaultRemoteFileSystem canceledDownloadRemote;
    seedDownloadSource(canceledDownloadRemote);
    seedLocalDownloadTarget(localDownloadTarget);
    result = file_replacement::downloadDirectoryReplacing(
        canceledDownloadRemote,
        remoteTarget,
        localDownloadTarget.string(),
        [](std::int64_t, std::int64_t) { return false; });
    require(!result.success, "canceled local directory replacement should fail");
    require(std::filesystem::is_regular_file(localDownloadTarget / "old-only.txt"),
        "canceled local directory replacement should preserve the original directory");
    require(!hasLocalReplacementArtifact(tempRoot),
        "canceled local directory replacement should clean the temporary directory");

    ReplacementFaultRemoteFileSystem failedDownloadRemote;
    seedDownloadSource(failedDownloadRemote);
    seedLocalDownloadTarget(localDownloadTarget);
    failedDownloadRemote.failTemporaryDownload = true;
    result = file_replacement::downloadDirectoryReplacing(
        failedDownloadRemote,
        remoteTarget,
        localDownloadTarget.string());
    require(!result.success, "temporary directory download failure should fail replacement");
    require(std::filesystem::is_regular_file(localDownloadTarget / "old-only.txt"),
        "directory download failure should preserve the original local directory");
    require(!hasLocalReplacementArtifact(tempRoot),
        "directory download failure should recursively clean the temporary local directory");

    ReplacementFaultRemoteFileSystem managerDownloadRemote;
    seedDownloadSource(managerDownloadRemote);
    seedLocalDownloadTarget(localDownloadTarget);
    TransferQueue downloadReplacementQueue;
    TransferJob downloadReplacementJob;
    downloadReplacementJob.id = "directory-download-replacement";
    downloadReplacementJob.name = "replacement-folder";
    downloadReplacementJob.kind = TransferJobKind::DirectoryReplacement;
    downloadReplacementJob.direction = TransferDirection::Download;
    downloadReplacementJob.localPath = localDownloadTarget.string();
    downloadReplacementJob.remotePath = remoteTarget;
    downloadReplacementJob.replaceExisting = true;
    downloadReplacementQueue.enqueue(downloadReplacementJob);
    TransferManager downloadReplacementManager(managerDownloadRemote, downloadReplacementQueue);
    downloadReplacementManager.processPending();
    require(downloadReplacementQueue.find(downloadReplacementJob.id)->status == TransferStatus::Completed,
        "directory download replacement task should be runnable through TransferManager");
    require(std::filesystem::is_regular_file(localDownloadTarget / "new-root.txt")
            && !std::filesystem::exists(localDownloadTarget / "old-only.txt"),
        "directory download replacement task should install the remote directory contents");
    require(!hasLocalReplacementArtifact(tempRoot),
        "manager directory download replacement should leave no local artifacts");

    std::filesystem::remove_all(tempRoot);
}

void checkExternalEditDocumentAndFileCache()
{
    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "dirbridge-external-edit-checks";
    std::filesystem::remove_all(tempRoot);

    FileCache cache(tempRoot);
    const FileCacheEntry firstEntry = cache.createEntry("session-a", "/remote/settings.json");
    const FileCacheEntry sameEntry = cache.createEntry("session-a", "/remote/settings.json");
    const FileCacheEntry sameNameDifferentPath = cache.createEntry("session-a", "/other/settings.json");
    const FileCacheEntry differentSession = cache.createEntry("session-b", "/remote/settings.json");

    require(firstEntry.documentId == sameEntry.documentId, "same external edit document should reuse cache key");
    require(firstEntry.directory != sameNameDifferentPath.directory, "same-name remote files need isolated cache directories");
    require(firstEntry.directory != differentSession.directory, "different sessions need isolated cache directories");
    require(firstEntry.workingFilePath.filename() == "settings.json", "editor cache working file should preserve the remote filename");
    require(firstEntry.workingFilePath.extension() == ".json", "cache working file should preserve a safe extension");

    const FileCacheEntry reservedNameEntry = cache.createEntry("session-a", "/remote/CON.txt");
    const FileCacheEntry invalidNameEntry = cache.createEntry("session-a", "/remote/a?b.cpp ");
    require(reservedNameEntry.workingFilePath.filename() == "content.txt", "Windows reserved editor cache filename should fall back safely");
    require(invalidNameEntry.workingFilePath.filename() == "a_b.cpp", "invalid editor cache filename characters and trailing spaces should be sanitized");

    FileCacheResult cacheResult = cache.prepareEntry(firstEntry);
    require(cacheResult.success, "external edit cache directory should be prepared");
    {
        std::ofstream temporaryFile(firstEntry.downloadTemporaryPath, std::ios::binary);
        temporaryFile << "first version";
    }
    cacheResult = cache.commitDownloadedFile(firstEntry);
    require(cacheResult.success, "downloaded editor cache file should commit safely");
    require(!std::filesystem::exists(firstEntry.downloadTemporaryPath), "committed temporary editor cache file should be moved");

    {
        std::ofstream temporaryFile(firstEntry.downloadTemporaryPath, std::ios::binary);
        temporaryFile << "fresh remote version";
    }
    cacheResult = cache.commitDownloadedFile(firstEntry);
    require(cacheResult.success, "stale editor cache should be preserved without blocking a fresh download");
    bool recoveredWorkingCopy = false;
    for (const std::filesystem::directory_entry &snapshot : std::filesystem::directory_iterator(firstEntry.snapshotsDirectory))
    {
        recoveredWorkingCopy = recoveredWorkingCopy || snapshot.path().filename().string().rfind("recovered-", 0) == 0;
    }
    require(recoveredWorkingCopy, "replaced editor cache working file should be retained as a recovery snapshot");

    std::filesystem::path firstSnapshot;
    cacheResult = cache.createUploadSnapshot(firstEntry, 1, firstSnapshot);
    require(cacheResult.success, "first editor upload snapshot should be created");
    {
        std::ifstream snapshot(firstSnapshot, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(snapshot)), std::istreambuf_iterator<char>());
        require(content == "fresh remote version", "editor upload snapshot content mismatch");
    }
    {
        std::ofstream workingFile(firstEntry.workingFilePath, std::ios::binary | std::ios::trunc);
        workingFile << "second version";
    }
    {
        std::ifstream snapshot(firstSnapshot, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(snapshot)), std::istreambuf_iterator<char>());
        require(content == "fresh remote version", "editor upload snapshot should remain immutable after a later save");
    }
    std::filesystem::path retriedSnapshot;
    cacheResult = cache.createUploadSnapshot(firstEntry, 1, retriedSnapshot);
    require(cacheResult.success && retriedSnapshot == firstSnapshot, "failed editor upload should be able to refresh the same snapshot version");
    {
        std::ifstream snapshot(retriedSnapshot, std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(snapshot)), std::istreambuf_iterator<char>());
        require(content == "second version", "refreshed editor upload snapshot should contain retry content");
    }

    ExternalEditDocument document("session-a", "/remote/settings.json", firstEntry);
    const RemoteFileRevision originalRevision{13, "2026-07-11 16:30:00", true};
    document.completeDownload(originalRevision);
    require(document.state() == ExternalEditState::EditingClean, "downloaded external edit document should be clean");

    document.markLocalFileChanged();
    const std::optional<std::uint64_t> firstUploadVersion = document.beginUpload();
    require(firstUploadVersion.has_value() && *firstUploadVersion == 1, "first changed external edit document should start upload version one");
    document.markLocalFileChanged();
    require(document.state() == ExternalEditState::Uploading, "new save during upload should keep visible uploading state");
    require(document.completeUpload(*firstUploadVersion, {14, "2026-07-11 16:31:00", true}), "first external edit upload should complete");
    require(document.state() == ExternalEditState::PendingUpload, "later save must remain pending after an older upload succeeds");

    const std::optional<std::uint64_t> secondUploadVersion = document.beginUpload();
    require(secondUploadVersion.has_value() && *secondUploadVersion == 2, "second external edit upload should use latest version");
    require(document.completeUpload(*secondUploadVersion, {15, "2026-07-11 16:32:00", true}), "second external edit upload should complete");
    require(document.state() == ExternalEditState::EditingClean, "latest external edit upload should become clean");

    document.markLocalFileChanged();
    const std::optional<std::uint64_t> failedUploadVersion = document.beginUpload();
    require(failedUploadVersion.has_value(), "changed external edit document should start a retryable upload");
    require(document.failUpload(*failedUploadVersion), "external edit upload failure should be accepted for active version");
    require(document.state() == ExternalEditState::UploadFailed && document.hasPendingUpload(), "failed external edit upload must preserve pending local content");
    document.markConflict();
    require(!document.beginUpload().has_value(), "conflicted external edit document must not auto-upload");
    document.resolveConflictForOverwrite();
    require(document.beginUpload().has_value(), "explicit conflict overwrite should make upload available again");

    cacheResult = cache.removeEntry(firstEntry);
    require(cacheResult.success, "cleaned external edit cache entry should remove safely");
    require(!std::filesystem::exists(firstEntry.directory), "removed external edit cache directory should not remain");
    std::filesystem::remove_all(tempRoot);
}

void checkTransferJob()
{
    TransferJob job;
    job.id = "job-1";
    job.name = "upload.txt";
    job.direction = TransferDirection::Upload;
    job.status = TransferStatus::Running;
    job.localPath = "C:/tmp/upload.txt";
    job.remotePath = "/home/testuser/remote_test/upload.txt";
    job.totalBytes = 200;
    job.transferredBytes = 50;

    require(toString(job.direction) == "upload", "transfer direction text mismatch");
    require(toString(job.status) == "running", "transfer status text mismatch");
    require(progressPercent(job) == 25, "transfer progress should be 25");

    job.transferredBytes = 300;
    require(progressPercent(job) == 100, "transfer progress should clamp to 100");

    job.totalBytes = -1;
    require(progressPercent(job) == 0, "unknown transfer size should report 0 percent");

    TransferJob download;
    download.id = "job-2";
    download.name = "readme.txt";
    download.direction = TransferDirection::Download;
    download.status = TransferStatus::Pending;
    download.localPath = "C:/tmp/readme.txt";
    download.remotePath = "/home/testuser/remote_test/readme.txt";
    download.totalBytes = 80;
    download.transferredBytes = 40;

    require(toString(download.direction) == "download", "download direction text mismatch");
    require(toString(download.status) == "pending", "pending status text mismatch");
    require(progressPercent(download) == 50, "download progress should be 50");

    download.status = TransferStatus::Completed;
    require(toString(download.status) == "completed", "completed status text mismatch");
    download.status = TransferStatus::Failed;
    require(toString(download.status) == "failed", "failed status text mismatch");
    download.status = TransferStatus::Canceled;
    require(toString(download.status) == "canceled", "canceled status text mismatch");
    download.status = TransferStatus::Canceling;
    require(toString(download.status) == "canceling", "canceling status text mismatch");
    download.kind = TransferJobKind::Directory;
    require(toString(download.kind) == "directory", "directory job kind text mismatch");
    download.kind = TransferJobKind::DirectoryReplacement;
    require(toString(download.kind) == "directory-replacement", "directory replacement job kind text mismatch");
    download.kind = TransferJobKind::DirectoryEntry;
    require(toString(download.kind) == "directory-entry", "directory entry job kind text mismatch");
    download.kind = TransferJobKind::File;
    require(toString(download.kind) == "file", "file job kind text mismatch");

    download.transferredBytes = -10;
    require(progressPercent(download) == 0, "negative transfer progress should clamp to 0");
}

void checkTransferQueueAndManager()
{
    SiteProfile profile;
    profile.name = "fake";
    profile.protocol = RemoteProtocol::Sftp;
    profile.host = "fake-host";
    profile.port = 22;
    profile.username = "testuser";
    profile.defaultRemotePath = "/home/testuser/remote_test";

    FakeRemoteFileSystem remote;
    RemoteOperationResult result = remote.connect(profile);
    require(result.success, "fake remote should connect for transfer manager");

    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "dirbridge-core-checks";
    std::filesystem::create_directories(tempRoot);
    const std::filesystem::path uploadSource = tempRoot / "queued-upload.txt";
    {
        std::ofstream output(uploadSource, std::ios::binary | std::ios::trunc);
        output << "queued upload\n";
    }

    TransferQueue queue;
    TransferJob canceled;
    canceled.id = "cancel-me";
    canceled.name = "cancel.txt";
    canceled.status = TransferStatus::Pending;
    queue.enqueue(canceled);
    require(queue.cancel(canceled.id, "user canceled"), "pending job should be cancelable");
    require(queue.find(canceled.id)->status == TransferStatus::Canceled, "canceled job status mismatch");
    require(queue.find(canceled.id)->errorMessage == "user canceled", "canceled job should keep cancellation reason");

    TransferJob upload;
    upload.id = "queued-upload";
    upload.name = "queued-upload.txt";
    upload.direction = TransferDirection::Upload;
    upload.status = TransferStatus::Pending;
    upload.localPath = uploadSource.string();
    upload.remotePath = "/home/testuser/remote_test/queued-upload.txt";
    upload.totalBytes = static_cast<std::int64_t>(std::filesystem::file_size(uploadSource));
    queue.enqueue(upload);

    TransferJob downloadMissing;
    downloadMissing.id = "missing-download";
    downloadMissing.name = "missing.txt";
    downloadMissing.direction = TransferDirection::Download;
    downloadMissing.status = TransferStatus::Pending;
    downloadMissing.remotePath = "/home/testuser/remote_test/missing.txt";
    downloadMissing.localPath = (tempRoot / "missing.txt").string();
    queue.enqueue(downloadMissing);

    int notifications = 0;
    int progressNotifications = 0;
    TransferManager manager(remote, queue);
    manager.setConcurrencyLimit(0);
    manager.setQueueChangedCallback([&notifications]() {
        ++notifications;
    });
    manager.setProgressCallback([&progressNotifications](const TransferJob &, std::int64_t transferredBytes, std::int64_t totalBytes) {
        if (totalBytes >= 0 && transferredBytes >= 0)
        {
            ++progressNotifications;
        }
        return true;
    });
    manager.processPending();

    const TransferJob *finishedUpload = queue.find(upload.id);
    require(finishedUpload != nullptr, "queued upload should remain in queue history");
    require(finishedUpload->status == TransferStatus::Completed, "queued upload should complete");
    require(progressPercent(*finishedUpload) == 100, "completed upload should report 100 percent");

    const TransferJob *failedDownload = queue.find(downloadMissing.id);
    require(failedDownload != nullptr, "failed download should remain in queue history");
    require(failedDownload->status == TransferStatus::Failed, "missing download should fail");
    require(!failedDownload->errorMessage.empty(), "failed download should keep error message");
    require(notifications >= 4, "manager should notify on running and terminal state changes");
    require(progressNotifications > 0, "manager should forward transfer progress");

    const TransferJob *retryJob = queue.retry(downloadMissing.id, "retry-missing-download");
    require(retryJob != nullptr, "failed job should be retryable");
    require(retryJob->status == TransferStatus::Pending, "retry job should be pending");
    require(retryJob->remotePath == downloadMissing.remotePath, "retry job should keep remote path");
    require(retryJob->localPath == downloadMissing.localPath, "retry job should keep local path");
    require(retryJob->sessionId == downloadMissing.sessionId, "retry job should keep session id");
    require(queue.retry(upload.id, "retry-completed-upload") == nullptr, "completed job should not be retryable");

    TransferQueue aggregateQueue;
    TransferJob directoryParent;
    directoryParent.id = "directory-parent";
    directoryParent.name = "folder";
    directoryParent.kind = TransferJobKind::Directory;
    directoryParent.status = TransferStatus::Pending;
    aggregateQueue.enqueue(directoryParent);
    TransferJob directoryChild = upload;
    directoryChild.id = "directory-child";
    directoryChild.parentId = directoryParent.id;
    directoryChild.status = TransferStatus::Pending;
    aggregateQueue.enqueue(directoryChild);
    TransferJob directoryEntry = directoryChild;
    directoryEntry.id = "directory-entry";
    directoryEntry.kind = TransferJobKind::DirectoryEntry;
    directoryEntry.parentId = directoryParent.id;
    aggregateQueue.enqueue(directoryEntry);
    TransferJob *nextAggregateJob = aggregateQueue.nextPending();
    require(nextAggregateJob != nullptr && nextAggregateJob->id == directoryChild.id, "directory parent should not be picked as executable transfer");
    directoryParent.status = TransferStatus::Failed;
    aggregateQueue.update(directoryParent);
    require(aggregateQueue.retry(directoryParent.id, "retry-directory-parent") == nullptr, "directory parent should not be retried directly");
    directoryParent.status = TransferStatus::Running;
    aggregateQueue.update(directoryParent);
    directoryEntry.status = TransferStatus::Running;
    aggregateQueue.update(directoryEntry);
    require(aggregateQueue.runningCount() == 0, "directory parent and display-only entries should not consume transfer concurrency");
    require(aggregateQueue.retry(directoryEntry.id, "retry-directory-entry") == nullptr,
        "display-only directory entries should not be retried independently");
    require(aggregateQueue.cancel(directoryParent.id, "cancel directory transfer"), "running directory parent should be cancelable");
    require(aggregateQueue.find(directoryParent.id)->status == TransferStatus::Canceled,
            "directory parent should become canceled immediately because it has no transfer worker");
    require(aggregateQueue.clearFinished() == 1, "canceled directory parent should be clearable");
    require(aggregateQueue.find(directoryParent.id) == nullptr, "clearFinished should remove canceled directory parent");
    require(aggregateQueue.find(directoryChild.id) != nullptr, "clearing directory parent should not implicitly remove unfinished children");

    TransferQueue externalQueue;
    TransferJob shellDownload;
    shellDownload.id = "shell-download";
    shellDownload.name = "readme.txt";
    shellDownload.direction = TransferDirection::Download;
    shellDownload.localPath = "Windows Shell";
    shellDownload.remotePath = "/home/testuser/remote_test/readme.txt";
    shellDownload.status = TransferStatus::Pending;
    shellDownload.externallyManaged = true;
    externalQueue.enqueue(shellDownload);
    require(externalQueue.nextPending() == nullptr,
        "externally managed shell downloads should not be executed by the normal transfer queue");
    shellDownload.status = TransferStatus::Failed;
    externalQueue.update(shellDownload);
    require(externalQueue.retry(shellDownload.id, "retry-shell-download") == nullptr,
        "externally managed shell downloads should not be retried without a Windows destination");

    const std::size_t removed = queue.clearFinished();
    require(removed >= 3, "clearFinished should remove completed, failed, and canceled jobs");
    require(queue.find(upload.id) == nullptr, "clearFinished should remove completed upload");
    require(queue.find(canceled.id) == nullptr, "clearFinished should remove canceled job");
    require(queue.find(downloadMissing.id) == nullptr, "clearFinished should remove failed download");

    TransferQueue cancelingQueue;
    TransferJob cancelingUpload;
    cancelingUpload.id = "cancel-running-upload";
    cancelingUpload.name = "cancel-running-upload.txt";
    cancelingUpload.direction = TransferDirection::Upload;
    cancelingUpload.status = TransferStatus::Pending;
    cancelingUpload.localPath = uploadSource.string();
    cancelingUpload.remotePath = "/home/testuser/remote_test/cancel-running-upload.txt";
    cancelingQueue.enqueue(cancelingUpload);

    TransferManager cancelingManager(remote, cancelingQueue);
    cancelingManager.setQueueChangedCallback([&cancelingQueue]() {
        TransferJob *job = cancelingQueue.find("cancel-running-upload");
        if (job != nullptr && job->status == TransferStatus::Running)
        {
            cancelingQueue.cancel(job->id, "cancel requested while running");
        }
    });
    cancelingManager.processPending();
    const TransferJob *canceledRunningJob = cancelingQueue.find(cancelingUpload.id);
    require(canceledRunningJob != nullptr, "running-cancel job should remain in queue");
    require(canceledRunningJob->status == TransferStatus::Canceled, "running cancel should finish as canceled");
    require(canceledRunningJob->errorMessage == "cancel requested while running", "running cancel should keep cancellation reason");

    TransferQueue missingSessionQueue;
    TransferJob missingSessionJob;
    missingSessionJob.id = "missing-session";
    missingSessionJob.name = "missing-session.txt";
    missingSessionJob.direction = TransferDirection::Download;
    missingSessionJob.status = TransferStatus::Pending;
    missingSessionJob.remotePath = "/home/testuser/remote_test/missing-session.txt";
    missingSessionJob.localPath = (tempRoot / "missing-session.txt").string();
    missingSessionQueue.enqueue(missingSessionJob);
    TransferManager missingSessionManager([](const TransferJob &) -> RemoteFileSystem * {
        return nullptr;
    }, missingSessionQueue);
    missingSessionManager.processPending();
    const TransferJob *failedMissingSessionJob = missingSessionQueue.find(missingSessionJob.id);
    require(failedMissingSessionJob != nullptr, "missing-session job should remain in queue");
    require(failedMissingSessionJob->status == TransferStatus::Failed, "missing remote session should fail job");
    require(failedMissingSessionJob->errorMessage == "remote session is not available", "missing session error message mismatch");
}

void checkSiteProfileAndSettingsStore()
{
    const nlohmann::json legacySiteJson = {
        {"id", "legacy-site"},
        {"name", "Legacy Site"},
        {"protocol", "sftp"},
        {"host", "legacy.example.test"},
        {"port", 22},
        {"username", "tester"},
        {"password", "legacy-secret"},
        {"defaultRemotePath", "/remote"},
        {"encoding", "UTF-8"}
    };
    const SiteProfile legacySite = legacySiteJson.get<SiteProfile>();
    require(legacySite.group.empty(), "legacy site without group should load with empty group");
    require(legacySite.password == "legacy-secret", "legacy plain-text site password should load");
    require(legacySite.fileTreeVisible, "legacy site should default to a visible file tree");
    require(!legacySite.sshRsaHostKeyCompatibility, "legacy site should disable ssh-rsa compatibility by default");

    SiteProfile groupedSite = legacySite;
    groupedSite.id = "grouped-site";
    groupedSite.group = "生产";
    groupedSite.password = "stored-secret";
    groupedSite.fileTreeVisible = false;
    groupedSite.sshRsaHostKeyCompatibility = true;
    nlohmann::json groupedJson = groupedSite;
    require(groupedJson.value("group", "") == "生产", "site group should serialize");
    require(!groupedJson.contains("password"), "site profile should not serialize plain password");
    require(groupedJson.value("passwordStorage", "") == passwordStorageScheme(), "site password storage scheme should serialize");
    require(groupedJson.contains("passwordProtected"), "site profile should serialize protected password");
    require(groupedJson.value("passwordProtected", "") != groupedSite.password, "protected password should not equal plain password");
    const SiteProfile roundTripSite = groupedJson.get<SiteProfile>();
    require(roundTripSite.group == "生产", "site group should round trip");
    require(roundTripSite.password == groupedSite.password, "protected password should round trip");
    require(!roundTripSite.fileTreeVisible, "site file-tree visibility should round trip");
    require(roundTripSite.sshRsaHostKeyCompatibility, "site ssh-rsa compatibility should round trip");

    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "dirbridge-settings-checks";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot);

    SiteStore siteStore(tempRoot / "sites.json");
    const std::vector<std::string> storedGroups = {"生产", "空分组"};
    siteStore.save({groupedSite}, storedGroups);
    {
        std::ifstream savedSites(siteStore.path());
        const std::string savedText((std::istreambuf_iterator<char>(savedSites)), std::istreambuf_iterator<char>());
        require(savedText.find("stored-secret") == std::string::npos, "site store should not persist plain password");
        require(savedText.find("passwordProtected") != std::string::npos, "site store should persist protected password");
    }
    const std::vector<SiteProfile> loadedSites = siteStore.load();
    require(loadedSites.size() == 1, "site store should reload saved site");
    require(loadedSites.front().group == "生产", "site store should preserve group");
    require(loadedSites.front().password == "stored-secret", "site store should decrypt saved password");
    require(!loadedSites.front().fileTreeVisible, "site store should preserve file-tree visibility");
    const std::vector<std::string> loadedGroups = siteStore.loadGroups();
    require(loadedGroups == storedGroups, "site store should preserve independent groups");

    nlohmann::json legacySites;
    legacySites["version"] = 1;
    legacySites["sites"] = std::vector<SiteProfile>{groupedSite};
    {
        std::ofstream output(siteStore.path(), std::ios::trunc);
        output << legacySites.dump(4);
    }
    require(siteStore.loadGroups().empty(), "legacy site store without groups should load an empty group list");

    SettingsStore settingsStore(tempRoot / "settings.json");
    UserSettings emptySettings = settingsStore.load();
    require(emptySettings.recentSessions.empty(), "missing settings file should load defaults");
    require(emptySettings.localFileTreeVisible, "missing settings file should show the local file tree by default");

    UserSettings settings;
    RecentSession recent;
    recent.siteId = groupedSite.id;
    recent.displayName = groupedSite.name;
    recent.lastRemotePath = "/remote/path";
    recent.lastOpenedAt = "2026-06-16T12:00:00";
    settings.recentSessions.push_back(recent);
    settings.localFileTreeVisible = false;
    settingsStore.save(settings);

    const UserSettings loadedSettings = settingsStore.load();
    require(loadedSettings.recentSessions.size() == 1, "settings store should reload recent sessions");
    require(loadedSettings.recentSessions.front().siteId == groupedSite.id, "recent session site id mismatch");
    require(loadedSettings.recentSessions.front().lastRemotePath == "/remote/path", "recent session path mismatch");
    require(!loadedSettings.localFileTreeVisible, "settings store should preserve local file-tree visibility");

    const std::filesystem::path legacySettingsPath = tempRoot / "legacy-settings.json";
    {
        std::ofstream output(legacySettingsPath, std::ios::trunc);
        output << R"({"version":1,"recentSessions":[]})";
    }
    require(SettingsStore(legacySettingsPath).load().localFileTreeVisible,
        "legacy settings without local file-tree visibility should default to visible");
}
}

int main()
{
    try
    {
        checkFakeRemoteFileSystem();
        checkFileReplacement();
        checkDirectoryReplacement();
        checkExternalEditDocumentAndFileCache();
        checkTransferJob();
        checkTransferQueueAndManager();
        checkSiteProfileAndSettingsStore();
    }
    catch (const std::exception &error)
    {
        std::cerr << "DirBridgeCoreChecks failed: " << error.what() << '\n';
        return 1;
    }

    std::cout << "DirBridgeCoreChecks passed\n";
    return 0;
}
