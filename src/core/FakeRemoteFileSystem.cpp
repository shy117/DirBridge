#include "core/FakeRemoteFileSystem.h"

#include <stdexcept>

RemoteOperationResult FakeRemoteFileSystem::connect(const SiteProfile &profile)
{
    m_profile = profile;
    m_connected = !profile.host.empty();

    if (!m_connected)
    {
        return {false, "host is required"};
    }

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

    const std::string currentPath = path.empty() ? "/" : path;
    return {
        {"home", currentPath + "/home", FileItemType::Directory, -1, "2026-06-05 09:00:00", "drwxr-xr-x", m_profile.username},
        {"var", currentPath + "/var", FileItemType::Directory, -1, "2026-06-05 09:01:00", "drwxr-xr-x", "root"},
        {"readme.txt", currentPath + "/readme.txt", FileItemType::File, 2048, "2026-06-05 09:02:00", "-rw-r--r--", m_profile.username},
        {"upload", currentPath + "/upload", FileItemType::Directory, -1, "2026-06-05 09:03:00", "drwxr-xr-x", m_profile.username}
    };
}
