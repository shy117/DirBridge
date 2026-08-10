#include "config/SiteProfile.h"
#include "config/SiteStore.h"
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

std::string argumentValue(int argc, char **argv, const std::string &name, bool required = false)
{
    for (int index = 1; index < argc; ++index)
    {
        if (argv[index] != name)
        {
            continue;
        }
        if (index + 1 >= argc)
        {
            throw std::runtime_error("missing value for argument: " + name);
        }
        return argv[index + 1];
    }
    if (required)
    {
        throw std::runtime_error("missing argument: " + name);
    }
    return {};
}

bool hasArgument(int argc, char **argv, const std::string &name)
{
    for (int index = 1; index < argc; ++index)
    {
        if (argv[index] == name)
        {
            return true;
        }
    }
    return false;
}

SiteProfile profileFromArgumentsOrEnvironment(int argc, char **argv)
{
    const std::string siteConfigPath = argumentValue(argc, argv, "--site-config");
    if (siteConfigPath.empty())
    {
        return profileFromEnvironment();
    }

    const std::string siteId = argumentValue(argc, argv, "--site-id", true);
    const std::vector<SiteProfile> sites = SiteStore(siteConfigPath).load();
    for (SiteProfile profile : sites)
    {
        if (profile.id != siteId)
        {
            continue;
        }
        const std::string remotePath = argumentValue(argc, argv, "--remote-path");
        if (!remotePath.empty())
        {
            profile.defaultRemotePath = remotePath;
        }
        return profile;
    }
    throw std::runtime_error("site id was not found in site config: " + siteId);
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

void removeRemoteDirectoryTree(CurlRemoteFileSystem &remote, const std::string &directoryPath)
{
    const std::vector<FileItem> items = remote.listDirectory(directoryPath);
    for (const FileItem &item : items)
    {
        if (item.type == FileItemType::Directory)
        {
            removeRemoteDirectoryTree(remote, item.path);
            continue;
        }

        const RemoteOperationResult removeResult = remote.removeFile(item.path);
        require(removeResult.success, "remove regression file failed: " + removeResult.message);
    }

    const RemoteOperationResult removeResult = remote.removeDirectory(directoryPath);
    require(removeResult.success, "remove regression directory failed: " + removeResult.message);
}

void runFtpCommandRegression(
    CurlRemoteFileSystem &remote,
    const std::string &basePath,
    bool checkPermissions)
{
    const std::string rootPath = joinRemotePath(basePath, uniqueName("dirbridge ftp regression "));
    const std::string sourcePath = joinRemotePath(rootPath, "source");
    const std::string targetPath = joinRemotePath(rootPath, "target");
    bool rootCreated = false;

    try
    {
        step("creating FTP regression root " + rootPath);
        RemoteOperationResult result = remote.createDirectory(rootPath);
        require(result.success, "create regression root failed: " + result.message);
        rootCreated = true;

        (void)remote.listDirectory(rootPath);
        result = remote.createDirectory(sourcePath);
        require(result.success, "create regression source failed: " + result.message);
        (void)remote.listDirectory(basePath);
        result = remote.createDirectory(targetPath);
        require(result.success, "create regression target failed: " + result.message);

        for (int index = 0; index < 4; ++index)
        {
            const std::string repeatedPath = joinRemotePath(rootPath, "repeat_" + std::to_string(index));
            (void)remote.listDirectory(index % 2 == 0 ? sourcePath : targetPath);
            result = remote.createDirectory(repeatedPath);
            require(result.success, "repeated directory create failed: " + result.message);
        }

        const std::string sourceFolderPath = joinRemotePath(sourcePath, "uploaded folder");
        const std::string targetFolderPath = joinRemotePath(targetPath, "uploaded folder");
        const std::string nestedPath = joinRemotePath(sourceFolderPath, "nested");
        const std::string nestedFilePath = joinRemotePath(nestedPath, "payload.txt");
        result = remote.createDirectory(sourceFolderPath);
        require(result.success, "create upload folder failed: " + result.message);
        result = remote.createDirectory(nestedPath);
        require(result.success, "create nested upload folder failed: " + result.message);
        result = remote.createFile(nestedFilePath);
        require(result.success, "create nested upload file failed: " + result.message);

        for (int index = 0; index < 2; ++index)
        {
            (void)remote.listDirectory(index == 0 ? sourcePath : basePath);
            result = remote.rename(sourceFolderPath, targetFolderPath);
            require(result.success, "cross-directory folder move failed: " + result.message);
            (void)remote.listDirectory(targetPath);
            result = remote.rename(targetFolderPath, sourceFolderPath);
            require(result.success, "cross-directory folder move-back failed: " + result.message);
        }

        const std::string sourceFilePath = joinRemotePath(sourcePath, "dragged file.txt");
        const std::string targetFilePath = joinRemotePath(targetPath, "dragged file.txt");
        result = remote.createFile(sourceFilePath);
        require(result.success, "create drag regression file failed: " + result.message);
        for (int index = 0; index < 2; ++index)
        {
            (void)remote.listDirectory(index == 0 ? targetPath : rootPath);
            result = remote.rename(sourceFilePath, targetFilePath);
            require(result.success, "cross-directory file move failed: " + result.message);
            (void)remote.listDirectory(sourcePath);
            result = remote.rename(targetFilePath, sourceFilePath);
            require(result.success, "cross-directory file move-back failed: " + result.message);
        }

        if (checkPermissions)
        {
            (void)remote.listDirectory(targetPath);
            result = remote.setPermissions(sourceFilePath, 0644);
            require(result.success, "FTP permission change failed: " + result.message);
        }

        removeRemoteDirectoryTree(remote, rootPath);
        rootCreated = false;
        const std::vector<FileItem> baseItems = remote.listDirectory(basePath);
        require(!containsPath(baseItems, rootPath, FileItemType::Directory), "FTP regression root was not removed");
    }
    catch (...)
    {
        if (rootCreated)
        {
            try
            {
                removeRemoteDirectoryTree(remote, rootPath);
            }
            catch (const std::exception &cleanupError)
            {
                std::cerr << "cleanup FTP regression root " << rootPath << ": " << cleanupError.what() << '\n';
            }
        }
        throw;
    }
}
}

int main(int argc, char **argv)
{
    CurlRemoteFileSystem remote;
    std::string uploadFilePath;
    std::string createdFilePath;
    std::string createdDirectoryPath;
    std::string renamedDirectoryPath;
    try
    {
        const SiteProfile profile = profileFromArgumentsOrEnvironment(argc, argv);
        const bool allowWrite = hasArgument(argc, argv, "--allow-write")
            || envValue("DIRBRIDGE_TEST_ALLOW_WRITE", false) == "1";

        const std::string stopAfterArgument = argumentValue(argc, argv, "--stop-after");
        const std::string stopAfter = stopAfterArgument.empty()
            ? envValue("DIRBRIDGE_TEST_STOP_AFTER", false)
            : stopAfterArgument;
        const std::string cleanPrefixArgument = argumentValue(argc, argv, "--clean-prefix");
        const std::string cleanPrefix = cleanPrefixArgument.empty()
            ? envValue("DIRBRIDGE_TEST_CLEAN_PREFIX", false)
            : cleanPrefixArgument;
        const bool cleanOnly = hasArgument(argc, argv, "--clean-only")
            || envValue("DIRBRIDGE_TEST_CLEAN_ONLY", false) == "1";
        const bool fileOnly = hasArgument(argc, argv, "--file-only")
            || envValue("DIRBRIDGE_TEST_FILE_ONLY", false) == "1";
        const bool unicodeName = hasArgument(argc, argv, "--unicode-name")
            || envValue("DIRBRIDGE_TEST_UNICODE_NAME", false) == "1";
        const bool ftpCommandRegression = hasArgument(argc, argv, "--ftp-command-regression")
            || envValue("DIRBRIDGE_TEST_FTP_COMMAND_REGRESSION", false) == "1";
        const bool checkFtpPermissions = hasArgument(argc, argv, "--check-ftp-permissions")
            || envValue("DIRBRIDGE_TEST_CHECK_FTP_PERMISSIONS", false) == "1";

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
            step("write checks skipped; use --allow-write to enable them");
            remote.disconnect();
            require(!remote.isConnected(), "remote did not report disconnected");
            step("DirBridgeRemoteCheck passed");
            return 0;
        }

        if (ftpCommandRegression)
        {
            require(
                profile.protocol == RemoteProtocol::Ftp || profile.protocol == RemoteProtocol::Ftps,
                "--ftp-command-regression requires an FTP or FTPS site");
            runFtpCommandRegression(remote, profile.defaultRemotePath, checkFtpPermissions);
            remote.disconnect();
            require(!remote.isConnected(), "remote did not report disconnected");
            step("DirBridgeRemoteCheck FTP command regression passed");
            return 0;
        }

        const std::string directoryName = uniqueName(unicodeName ? u8"dirbridge 删除验证 " : "dirbridge check ");
        const std::string directoryPath = joinRemotePath(profile.defaultRemotePath, directoryName);
        const std::string targetDirectoryPath = directoryPath + "_renamed";
        uploadFilePath = joinRemotePath(profile.defaultRemotePath, directoryName + ".txt");
        createdFilePath = joinRemotePath(profile.defaultRemotePath, directoryName + "_empty.txt");

        if (fileOnly)
        {
            const std::filesystem::path tempRoot = std::filesystem::temp_directory_path() / "dirbridge-remote-check";
            std::filesystem::create_directories(tempRoot);
            const std::filesystem::path uploadSource = tempRoot / (uniqueName("dirbridge local ") + ".txt");
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

            const std::filesystem::path downloadTarget = tempRoot / (uniqueName("dirbridge download ") + ".txt");
            step("downloading file " + uploadFilePath);
            result = remote.downloadFile(uploadFilePath, downloadTarget.string());
            require(result.success, "download file failed: " + result.message);
            require(std::filesystem::is_regular_file(downloadTarget), "downloaded file is missing locally");

            step("removing file " + uploadFilePath);
            result = remote.removeFile(uploadFilePath);
            require(result.success, "remove uploaded file failed: " + result.message);
            items = remote.listDirectory(profile.defaultRemotePath);
            require(!containsPath(items, uploadFilePath, FileItemType::File), "removed file is still listed");
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
        const std::filesystem::path uploadSource = tempRoot / (uniqueName("dirbridge local ") + ".txt");
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

        const std::filesystem::path downloadTarget = tempRoot / (uniqueName("dirbridge download ") + ".txt");
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
        result = remote.removeFile(uploadFilePath);
        require(result.success, "remove uploaded file failed: " + result.message);

        step("removing empty file " + createdFilePath);
        result = remote.removeFile(createdFilePath);
        require(result.success, "remove created empty file failed: " + result.message);

        step("removing directory " + renamedDirectoryPath);
        result = remote.removeDirectory(renamedDirectoryPath);
        require(result.success, "remove renamed directory failed: " + result.message);

        items = remote.listDirectory(profile.defaultRemotePath);
        require(!containsPath(items, uploadFilePath, FileItemType::File), "removed file is still listed");
        require(!containsPath(items, createdFilePath, FileItemType::File), "removed empty file is still listed");
        require(!containsPath(items, renamedDirectoryPath, FileItemType::Directory), "removed directory is still listed");
        uploadFilePath.clear();
        createdFilePath.clear();
        renamedDirectoryPath.clear();

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
