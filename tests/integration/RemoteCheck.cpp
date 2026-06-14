#include "config/SiteProfile.h"
#include "protocol/CurlRemoteFileSystem.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
std::string envValue(const char *name, bool required = true)
{
    const char *value = std::getenv(name);
    if (value == nullptr || std::string(value).empty())
    {
        if (required)
        {
            throw std::runtime_error(std::string("missing environment variable: ") + name);
        }
        return {};
    }
    return value;
}

std::string joinRemotePath(std::string directory, const std::string &name)
{
    if (directory.empty())
    {
        directory = "/";
    }
    if (directory.front() != '/')
    {
        directory.insert(directory.begin(), '/');
    }
    if (directory.back() != '/')
    {
        directory.push_back('/');
    }
    return directory + name;
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

void require(bool condition, const std::string &message)
{
    if (!condition)
    {
        throw std::runtime_error(message);
    }
}

SiteProfile profileFromEnvironment()
{
    SiteProfile profile;
    profile.protocol = remoteProtocolFromString(envValue("DIRBRIDGE_TEST_PROTOCOL"));
    profile.host = envValue("DIRBRIDGE_TEST_HOST");
    profile.port = static_cast<std::uint16_t>(std::stoi(envValue("DIRBRIDGE_TEST_PORT")));
    profile.username = envValue("DIRBRIDGE_TEST_USER");
    profile.password = envValue("DIRBRIDGE_TEST_PASSWORD");
    profile.defaultRemotePath = envValue("DIRBRIDGE_TEST_PATH");
    profile.name = "remote-check";
    profile.encoding = "UTF-8";
    return profile;
}

std::string uniqueName(const std::string &prefix)
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    return prefix + std::to_string(millis);
}

void printItems(const std::vector<FileItem> &items)
{
    std::cout << "listed " << items.size() << " item(s)\n";
    for (const FileItem &item : items)
    {
        std::cout << "  " << item.path << " [" << fileItemTypeName(item.type) << "]\n";
    }
    std::cout.flush();
}

void step(const std::string &message)
{
    std::cout << message << '\n';
    std::cout.flush();
}

bool startsWith(const std::string &value, const std::string &prefix)
{
    return value.rfind(prefix, 0) == 0;
}

void cleanupPrefix(CurlRemoteFileSystem &remote, const std::string &basePath, const std::string &prefix)
{
    if (prefix.empty())
    {
        return;
    }

    step("cleaning remote items with prefix " + prefix);
    std::vector<FileItem> items = remote.listDirectory(basePath);
    for (const FileItem &item : items)
    {
        if (item.type == FileItemType::File && startsWith(item.name, prefix))
        {
            const RemoteOperationResult result = remote.remove(item.path);
            step("cleanup file " + item.path + ": " + result.message);
        }
    }

    items = remote.listDirectory(basePath);
    for (const FileItem &item : items)
    {
        if (item.type == FileItemType::Directory && startsWith(item.name, prefix))
        {
            const RemoteOperationResult result = remote.remove(item.path);
            step("cleanup directory " + item.path + ": " + result.message);
        }
    }
}
}

int main()
{
    CurlRemoteFileSystem remote;
    std::string uploadFilePath;
    std::string createdFilePath;
    std::string createdDirectoryPath;
    std::string renamedDirectoryPath;
    try
    {
        const SiteProfile profile = profileFromEnvironment();
        const bool allowWrite = envValue("DIRBRIDGE_TEST_ALLOW_WRITE", false) == "1";

        const std::string stopAfter = envValue("DIRBRIDGE_TEST_STOP_AFTER", false);
        const std::string cleanPrefix = envValue("DIRBRIDGE_TEST_CLEAN_PREFIX", false);
        const bool cleanOnly = envValue("DIRBRIDGE_TEST_CLEAN_ONLY", false) == "1";
        const bool fileOnly = envValue("DIRBRIDGE_TEST_FILE_ONLY", false) == "1";

        step("connecting " + toString(profile.protocol) + "://" + profile.host + ':' + std::to_string(profile.port)
            + " path=" + profile.defaultRemotePath);
        RemoteOperationResult result = remote.connect(profile);
        require(result.success, "connect failed: " + result.message);
        require(remote.isConnected(), "remote did not report connected");

        step("listing base directory");
        std::vector<FileItem> items = remote.listDirectory(profile.defaultRemotePath);
        printItems(items);

        if (!cleanPrefix.empty())
        {
            cleanupPrefix(remote, profile.defaultRemotePath, cleanPrefix);
            if (cleanOnly)
            {
                remote.disconnect();
                require(!remote.isConnected(), "remote did not report disconnected");
                step("DirBridgeRemoteCheck cleanup passed");
                return 0;
            }
        }

        const std::string missingPath = joinRemotePath(profile.defaultRemotePath, uniqueName("missing_"));
        bool missingPathFailed = false;
        try
        {
            (void)remote.listDirectory(missingPath);
        }
        catch (const std::exception &error)
        {
            missingPathFailed = true;
            step(std::string("missing path produced expected error: ") + error.what());
        }
        require(missingPathFailed, "missing remote path should fail");

        if (!allowWrite)
        {
            step("write checks skipped; set DIRBRIDGE_TEST_ALLOW_WRITE=1 to enable them");
            remote.disconnect();
            require(!remote.isConnected(), "remote did not report disconnected");
            step("DirBridgeRemoteCheck passed");
            return 0;
        }

        const std::string directoryName = uniqueName("dirbridge_check_");
        const std::string directoryPath = joinRemotePath(profile.defaultRemotePath, directoryName);
        const std::string targetDirectoryPath = directoryPath + "_renamed";
        uploadFilePath = joinRemotePath(profile.defaultRemotePath, directoryName + ".txt");
        createdFilePath = joinRemotePath(profile.defaultRemotePath, directoryName + "_empty.txt");

        if (fileOnly)
        {
            const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "dirbridge-remote-check";
            std::filesystem::create_directories(tempRoot);
            const std::filesystem::path uploadSource = tempRoot / (directoryName + ".txt");
            {
                std::ofstream output(uploadSource, std::ios::binary | std::ios::trunc);
                output << "DirBridge remote file-only check\n";
            }

            step("uploading file " + uploadFilePath);
            result = remote.uploadFile(uploadSource.string(), uploadFilePath);
            require(result.success, "upload file failed: " + result.message);
            step("listing after upload");
            items = remote.listDirectory(profile.defaultRemotePath);
            require(containsPath(items, uploadFilePath, FileItemType::File), "uploaded file is not listed");

            const std::filesystem::path downloadTarget = tempRoot / (directoryName + ".download.txt");
            step("downloading file " + uploadFilePath);
            result = remote.downloadFile(uploadFilePath, downloadTarget.string());
            require(result.success, "download file failed: " + result.message);
            require(std::filesystem::is_regular_file(downloadTarget), "downloaded file is missing locally");

            step("removing file " + uploadFilePath);
            result = remote.remove(uploadFilePath);
            require(result.success, "remove uploaded file failed: " + result.message);
            uploadFilePath.clear();

            remote.disconnect();
            require(!remote.isConnected(), "remote did not report disconnected");
            step("DirBridgeRemoteCheck passed");
            return 0;
        }

        step("creating directory " + directoryPath);
        result = remote.createDirectory(directoryPath);
        require(result.success, "create directory failed: " + result.message);
        createdDirectoryPath = directoryPath;
        items = remote.listDirectory(profile.defaultRemotePath);
        require(containsPath(items, directoryPath, FileItemType::Directory), "created directory is not listed");
        if (stopAfter == "create")
        {
            remote.disconnect();
            step("DirBridgeRemoteCheck stopped after create");
            return 0;
        }

        step("checking duplicate create error");
        result = remote.createDirectory(directoryPath);
        require(!result.success, "duplicate create should fail");
        step("duplicate create produced expected error: " + result.message);

        step("creating empty file " + createdFilePath);
        result = remote.createFile(createdFilePath);
        require(result.success, "create empty file failed: " + result.message);
        items = remote.listDirectory(profile.defaultRemotePath);
        require(containsPath(items, createdFilePath, FileItemType::File), "created empty file is not listed");

        step("checking duplicate empty file create error");
        result = remote.createFile(createdFilePath);
        require(!result.success, "duplicate empty file create should fail");
        step("duplicate empty file create produced expected error: " + result.message);

        step("renaming directory " + directoryPath + " -> " + targetDirectoryPath);
        result = remote.rename(directoryPath, targetDirectoryPath);
        require(result.success, "rename directory failed: " + result.message);
        createdDirectoryPath.clear();
        renamedDirectoryPath = targetDirectoryPath;
        items = remote.listDirectory(profile.defaultRemotePath);
        require(containsPath(items, renamedDirectoryPath, FileItemType::Directory), "renamed directory is not listed");
        if (stopAfter == "rename")
        {
            remote.disconnect();
            step("DirBridgeRemoteCheck stopped after rename");
            return 0;
        }

        const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "dirbridge-remote-check";
        std::filesystem::create_directories(tempRoot);
        const std::filesystem::path uploadSource = tempRoot / (directoryName + ".txt");
        {
            std::ofstream output(uploadSource, std::ios::binary | std::ios::trunc);
            output << "DirBridge remote check\n";
        }

        step("uploading file " + uploadFilePath);
        result = remote.uploadFile(uploadSource.string(), uploadFilePath);
        require(result.success, "upload file failed: " + result.message);
        step("listing after upload");
        items = remote.listDirectory(profile.defaultRemotePath);
        require(containsPath(items, uploadFilePath, FileItemType::File), "uploaded file is not listed");
        if (stopAfter == "upload")
        {
            remote.disconnect();
            step("DirBridgeRemoteCheck stopped after upload");
            return 0;
        }

        const std::filesystem::path downloadTarget = tempRoot / (directoryName + ".download.txt");
        step("downloading file " + uploadFilePath);
        result = remote.downloadFile(uploadFilePath, downloadTarget.string());
        require(result.success, "download file failed: " + result.message);
        require(std::filesystem::is_regular_file(downloadTarget), "downloaded file is missing locally");
        if (stopAfter == "download")
        {
            remote.disconnect();
            step("DirBridgeRemoteCheck stopped after download");
            return 0;
        }

        step("removing file " + uploadFilePath);
        result = remote.remove(uploadFilePath);
        require(result.success, "remove uploaded file failed: " + result.message);

        uploadFilePath.clear();

        step("removing empty file " + createdFilePath);
        result = remote.remove(createdFilePath);
        require(result.success, "remove created empty file failed: " + result.message);
        createdFilePath.clear();

        step("removing directory " + renamedDirectoryPath);
        result = remote.remove(renamedDirectoryPath);
        require(result.success, "remove renamed directory failed: " + result.message);
        renamedDirectoryPath.clear();

        items = remote.listDirectory(profile.defaultRemotePath);
        require(!containsPath(items, uploadFilePath, FileItemType::File), "removed file is still listed");
        require(!containsPath(items, renamedDirectoryPath, FileItemType::Directory), "removed directory is still listed");

        remote.disconnect();
        require(!remote.isConnected(), "remote did not report disconnected");
    }
    catch (const std::exception &error)
    {
        std::cerr << "DirBridgeRemoteCheck failed: " << error.what() << '\n';
        if (remote.isConnected())
        {
            if (!uploadFilePath.empty())
            {
                const RemoteOperationResult cleanup = remote.remove(uploadFilePath);
                std::cerr << "cleanup file " << uploadFilePath << ": " << cleanup.message << '\n';
            }
            if (!createdFilePath.empty())
            {
                const RemoteOperationResult cleanup = remote.remove(createdFilePath);
                std::cerr << "cleanup file " << createdFilePath << ": " << cleanup.message << '\n';
            }
            if (!createdDirectoryPath.empty())
            {
                const RemoteOperationResult cleanup = remote.remove(createdDirectoryPath);
                std::cerr << "cleanup directory " << createdDirectoryPath << ": " << cleanup.message << '\n';
            }
            if (!renamedDirectoryPath.empty())
            {
                const RemoteOperationResult cleanup = remote.remove(renamedDirectoryPath);
                std::cerr << "cleanup directory " << renamedDirectoryPath << ": " << cleanup.message << '\n';
            }
        }
        return 1;
    }

    step("DirBridgeRemoteCheck passed");
    return 0;
}
