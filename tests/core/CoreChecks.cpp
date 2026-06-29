#include "config/SettingsStore.h"
#include "config/SiteProfile.h"
#include "config/SiteStore.h"
#include "config/UserSettings.h"
#include "core/FakeRemoteFileSystem.h"
#include "core/TransferJob.h"
#include "core/TransferManager.h"
#include "core/TransferQueue.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
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
    require(result.success, "download file should succeed");
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
    TransferManager manager(remote, queue);
    manager.setConcurrencyLimit(0);
    manager.setQueueChangedCallback([&notifications]() {
        ++notifications;
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
    TransferJob *nextAggregateJob = aggregateQueue.nextPending();
    require(nextAggregateJob != nullptr && nextAggregateJob->id == directoryChild.id, "directory parent should not be picked as executable transfer");
    directoryParent.status = TransferStatus::Failed;
    aggregateQueue.update(directoryParent);
    require(aggregateQueue.retry(directoryParent.id, "retry-directory-parent") == nullptr, "directory parent should not be retried directly");
    directoryParent.status = TransferStatus::Running;
    aggregateQueue.update(directoryParent);
    require(aggregateQueue.runningCount() == 0, "directory parent should not consume transfer concurrency");

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
        {"password", ""},
        {"defaultRemotePath", "/remote"},
        {"encoding", "UTF-8"}
    };
    const SiteProfile legacySite = legacySiteJson.get<SiteProfile>();
    require(legacySite.group.empty(), "legacy site without group should load with empty group");

    SiteProfile groupedSite = legacySite;
    groupedSite.id = "grouped-site";
    groupedSite.group = "生产";
    nlohmann::json groupedJson = groupedSite;
    require(groupedJson.value("group", "") == "生产", "site group should serialize");
    const SiteProfile roundTripSite = groupedJson.get<SiteProfile>();
    require(roundTripSite.group == "生产", "site group should round trip");

    const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "dirbridge-settings-checks";
    std::filesystem::remove_all(tempRoot);
    std::filesystem::create_directories(tempRoot);

    SiteStore siteStore(tempRoot / "sites.json");
    siteStore.save({groupedSite});
    const std::vector<SiteProfile> loadedSites = siteStore.load();
    require(loadedSites.size() == 1, "site store should reload saved site");
    require(loadedSites.front().group == "生产", "site store should preserve group");

    SettingsStore settingsStore(tempRoot / "settings.json");
    UserSettings emptySettings = settingsStore.load();
    require(emptySettings.recentSessions.empty(), "missing settings file should load defaults");

    UserSettings settings;
    RecentSession recent;
    recent.siteId = groupedSite.id;
    recent.displayName = groupedSite.name;
    recent.lastRemotePath = "/remote/path";
    recent.lastOpenedAt = "2026-06-16T12:00:00";
    settings.recentSessions.push_back(recent);
    settingsStore.save(settings);

    const UserSettings loadedSettings = settingsStore.load();
    require(loadedSettings.recentSessions.size() == 1, "settings store should reload recent sessions");
    require(loadedSettings.recentSessions.front().siteId == groupedSite.id, "recent session site id mismatch");
    require(loadedSettings.recentSessions.front().lastRemotePath == "/remote/path", "recent session path mismatch");
}
}

int main()
{
    try
    {
        checkFakeRemoteFileSystem();
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
