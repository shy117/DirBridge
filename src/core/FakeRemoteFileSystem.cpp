#include "core/FakeRemoteFileSystem.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace
{
std::string normalizePath(std::string path)
{
    if (path.empty())
    {
        return "/";
    }
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.front() != '/')
    {
        path.insert(path.begin(), '/');
    }
    while (path.size() > 1 && path.back() == '/')
    {
        path.pop_back();
    }
    return path;
}

std::string parentPath(const std::string &path)
{
    const std::string normalized = normalizePath(path);
    if (normalized == "/")
    {
        return "/";
    }

    const std::size_t slashIndex = normalized.find_last_of('/');
    return slashIndex == 0 ? "/" : normalized.substr(0, slashIndex);
}

std::string baseName(const std::string &path)
{
    const std::string normalized = normalizePath(path);
    if (normalized == "/")
    {
        return "/";
    }

    return normalized.substr(normalized.find_last_of('/') + 1);
}

bool isDirectChild(const std::string &parent, const std::string &candidate)
{
    const std::string normalizedParent = normalizePath(parent);
    const std::string normalizedCandidate = normalizePath(candidate);
    if (normalizedCandidate == normalizedParent)
    {
        return false;
    }

    if (parentPath(normalizedCandidate) != normalizedParent)
    {
        return false;
    }

    return true;
}

FileItem makeDirectory(const std::string &path, const std::string &owner)
{
    return {baseName(path), normalizePath(path), FileItemType::Directory, -1, "2026-06-07 09:00:00", "drwxr-xr-x", owner};
}

FileItem makeFile(const std::string &path, std::int64_t size, const std::string &owner)
{
    return {baseName(path), normalizePath(path), FileItemType::File, size, "2026-06-07 09:00:00", "-rw-r--r--", owner};
}
}

RemoteOperationResult FakeRemoteFileSystem::connect(const SiteProfile &profile)
{
    m_profile = profile;
    m_connected = !profile.host.empty();

    if (!m_connected)
    {
        return {false, "host is required"};
    }

    const std::string owner = profile.username.empty() ? "user" : profile.username;
    m_items.clear();
    m_items.emplace("/", makeDirectory("/", owner));
    m_items.emplace("/home", makeDirectory("/home", owner));
    m_items.emplace("/home/testuser", makeDirectory("/home/testuser", owner));
    m_items.emplace("/home/testuser/remote_test", makeDirectory("/home/testuser/remote_test", owner));
    m_items.emplace("/home/testuser/remote_test/download", makeDirectory("/home/testuser/remote_test/download", owner));
    m_items.emplace("/home/testuser/remote_test/upload", makeDirectory("/home/testuser/remote_test/upload", owner));
    m_items.emplace("/home/testuser/remote_test/edit", makeDirectory("/home/testuser/remote_test/edit", owner));
    m_items.emplace("/home/testuser/remote_test/readme.txt", makeFile("/home/testuser/remote_test/readme.txt", 12, owner));

    return {true, "mock connection ready"};
}

void FakeRemoteFileSystem::disconnect()
{
    m_connected = false;
}

bool FakeRemoteFileSystem::isConnected() const
{
    return m_connected;
}

std::vector<FileItem> FakeRemoteFileSystem::listDirectory(const std::string &path)
{
    if (!m_connected)
    {
        throw std::runtime_error("remote session is not connected");
    }

    const std::string currentPath = normalizePath(path);
    const auto currentItem = m_items.find(currentPath);
    if (currentItem == m_items.end() || currentItem->second.type != FileItemType::Directory)
    {
        throw std::runtime_error("remote directory does not exist: " + currentPath);
    }

    std::vector<FileItem> children;
    for (const auto &[candidatePath, item] : m_items)
    {
        if (isDirectChild(currentPath, candidatePath))
        {
            children.push_back(item);
        }
    }

    std::sort(children.begin(), children.end(), [](const FileItem &left, const FileItem &right) {
        if (left.type != right.type)
        {
            return left.type == FileItemType::Directory;
        }
        return left.name < right.name;
    });
    return children;
}

RemoteOperationResult FakeRemoteFileSystem::createDirectory(const std::string &path)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedPath = normalizePath(path);
    if (m_items.find(normalizedPath) != m_items.end())
    {
        return {false, "remote item already exists"};
    }
    const auto parent = m_items.find(parentPath(normalizedPath));
    if (parent == m_items.end() || parent->second.type != FileItemType::Directory)
    {
        return {false, "remote parent directory does not exist"};
    }

    const std::string owner = m_profile.username.empty() ? "user" : m_profile.username;
    m_items.emplace(normalizedPath, makeDirectory(normalizedPath, owner));
    return {true, "directory created"};
}

RemoteOperationResult FakeRemoteFileSystem::createFile(const std::string &path)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedPath = normalizePath(path);
    if (m_items.find(normalizedPath) != m_items.end())
    {
        return {false, "remote item already exists"};
    }
    const auto parent = m_items.find(parentPath(normalizedPath));
    if (parent == m_items.end() || parent->second.type != FileItemType::Directory)
    {
        return {false, "remote parent directory does not exist"};
    }

    const std::string owner = m_profile.username.empty() ? "user" : m_profile.username;
    m_items.emplace(normalizedPath, makeFile(normalizedPath, 0, owner));
    return {true, "file created"};
}

RemoteOperationResult FakeRemoteFileSystem::remove(const std::string &path)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedPath = normalizePath(path);
    const auto item = m_items.find(normalizedPath);
    if (item == m_items.end() || normalizedPath == "/")
    {
        return {false, "remote item does not exist"};
    }

    if (item->second.type == FileItemType::Directory)
    {
        const auto child = std::find_if(m_items.begin(), m_items.end(), [&normalizedPath](const auto &entry) {
            return isDirectChild(normalizedPath, entry.first);
        });
        if (child != m_items.end())
        {
            return {false, "remote directory is not empty"};
        }
    }

    m_items.erase(item);
    return {true, "remote item removed"};
}

RemoteOperationResult FakeRemoteFileSystem::rename(const std::string &sourcePath, const std::string &targetPath)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedSource = normalizePath(sourcePath);
    const std::string normalizedTarget = normalizePath(targetPath);
    const auto sourceItem = m_items.find(normalizedSource);
    if (sourceItem == m_items.end() || normalizedSource == "/")
    {
        return {false, "remote source item does not exist"};
    }
    if (m_items.find(normalizedTarget) != m_items.end())
    {
        return {false, "remote target item already exists"};
    }
    const auto targetParent = m_items.find(parentPath(normalizedTarget));
    if (targetParent == m_items.end() || targetParent->second.type != FileItemType::Directory)
    {
        return {false, "remote target parent directory does not exist"};
    }

    FileItem renamed = sourceItem->second;
    renamed.name = baseName(normalizedTarget);
    renamed.path = normalizedTarget;
    const bool sourceIsDirectory = sourceItem->second.type == FileItemType::Directory;
    m_items.erase(sourceItem);
    m_items.emplace(normalizedTarget, renamed);

    if (sourceIsDirectory)
    {
        std::vector<std::pair<std::string, FileItem>> descendants;
        for (const auto &[candidatePath, item] : m_items)
        {
            if (candidatePath.rfind(normalizedSource + "/", 0) == 0)
            {
                FileItem moved = item;
                moved.path = normalizedTarget + candidatePath.substr(normalizedSource.size());
                descendants.emplace_back(candidatePath, std::move(moved));
            }
        }

        for (const auto &[oldPath, moved] : descendants)
        {
            m_items.erase(oldPath);
            m_items.emplace(moved.path, moved);
        }
    }

    return {true, "remote item renamed"};
}

RemoteOperationResult FakeRemoteFileSystem::uploadFile(const std::string &localPath, const std::string &remotePath)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::filesystem::path source(localPath);
    if (!std::filesystem::is_regular_file(source))
    {
        return {false, "local file does not exist"};
    }

    const std::string normalizedRemotePath = normalizePath(remotePath);
    if (m_items.find(parentPath(normalizedRemotePath)) == m_items.end())
    {
        return {false, "remote parent directory does not exist"};
    }

    const std::string owner = m_profile.username.empty() ? "user" : m_profile.username;
    m_items[normalizedRemotePath] = makeFile(normalizedRemotePath, static_cast<std::int64_t>(std::filesystem::file_size(source)), owner);
    return {true, "file uploaded"};
}

RemoteOperationResult FakeRemoteFileSystem::downloadFile(const std::string &remotePath, const std::string &localPath)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedRemotePath = normalizePath(remotePath);
    const auto remoteItem = m_items.find(normalizedRemotePath);
    if (remoteItem == m_items.end() || remoteItem->second.type != FileItemType::File)
    {
        return {false, "remote file does not exist"};
    }

    const std::filesystem::path target(localPath);
    if (target.has_parent_path())
    {
        std::filesystem::create_directories(target.parent_path());
    }

    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        return {false, "local file cannot be opened for writing"};
    }

    output << "fake remote file: " << normalizedRemotePath << '\n';
    return {true, "file downloaded"};
}
