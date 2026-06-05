#include "protocol/CurlRemoteFileSystem.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <sstream>
#include <stdexcept>

#include <curl/curl.h>

namespace
{
struct CurlHandleDeleter
{
    void operator()(CURL *handle) const
    {
        if (handle != nullptr)
        {
            curl_easy_cleanup(handle);
        }
    }
};

using CurlHandle = std::unique_ptr<CURL, CurlHandleDeleter>;

size_t writeStringCallback(char *data, size_t size, size_t count, void *userData)
{
    const size_t bytes = size * count;
    auto *buffer = static_cast<std::string *>(userData);
    buffer->append(data, bytes);
    return bytes;
}

std::string trimCopy(std::string value)
{
    auto isSpace = [](unsigned char character) {
        return std::isspace(character) != 0;
    };

    value.erase(value.begin(), std::find_if_not(value.begin(), value.end(), isSpace));
    value.erase(std::find_if_not(value.rbegin(), value.rend(), isSpace).base(), value.end());
    return value;
}

std::string normalizeRemotePath(std::string path)
{
    if (path.empty())
    {
        return "/";
    }
    if (path.front() != '/')
    {
        path.insert(path.begin(), '/');
    }
    return path;
}

std::string directoryUrl(const SiteProfile &profile, const std::string &path)
{
    std::ostringstream stream;
    stream << toString(profile.protocol) << "://" << profile.host << ':' << profile.port;
    stream << normalizeRemotePath(path);
    if (stream.str().back() != '/')
    {
        stream << '/';
    }
    return stream.str();
}

std::string joinRemotePath(const std::string &directory, const std::string &name)
{
    std::string path = normalizeRemotePath(directory);
    if (path.back() != '/')
    {
        path.push_back('/');
    }
    return path + name;
}

std::vector<FileItem> parseNameList(const std::string &listing, const std::string &directory)
{
    std::vector<FileItem> items;
    std::istringstream stream(listing);
    std::string line;

    while (std::getline(stream, line))
    {
        std::string name = trimCopy(line);
        if (name.empty() || name == "." || name == "..")
        {
            continue;
        }

        const bool directoryHint = name.back() == '/';
        if (directoryHint)
        {
            name.pop_back();
        }

        FileItem item;
        item.name = name;
        item.path = joinRemotePath(directory, name);
        item.type = directoryHint ? FileItemType::Directory : FileItemType::Other;
        item.size = -1;
        item.modifiedTime = "";
        item.permissions = "";
        item.owner = "";
        items.push_back(std::move(item));
    }

    return items;
}

void setCurlOption(CURL *handle, CURLoption option, const char *value)
{
    const CURLcode code = curl_easy_setopt(handle, option, value);
    if (code != CURLE_OK)
    {
        throw std::runtime_error(curl_easy_strerror(code));
    }
}

void setCurlLongOption(CURL *handle, CURLoption option, long value)
{
    const CURLcode code = curl_easy_setopt(handle, option, value);
    if (code != CURLE_OK)
    {
        throw std::runtime_error(curl_easy_strerror(code));
    }
}
}

RemoteOperationResult CurlRemoteFileSystem::connect(const SiteProfile &profile)
{
    if (profile.host.empty())
    {
        return {false, "host is required"};
    }

    if (profile.protocol != RemoteProtocol::Ftp
        && profile.protocol != RemoteProtocol::Ftps
        && profile.protocol != RemoteProtocol::Sftp)
    {
        return {false, "only ftp, ftps and sftp are supported for directory browsing"};
    }

    m_profile = profile;
    m_connected = true;
    return {true, "connection profile accepted"};
}

void CurlRemoteFileSystem::disconnect()
{
    m_connected = false;
}

bool CurlRemoteFileSystem::isConnected() const
{
    return m_connected;
}

std::vector<FileItem> CurlRemoteFileSystem::listDirectory(const std::string &path)
{
    if (!m_connected)
    {
        throw std::runtime_error("remote session is not connected");
    }

    CurlHandle handle(curl_easy_init());
    if (!handle)
    {
        throw std::runtime_error("failed to initialize libcurl easy handle");
    }

    std::string listing;
    char errorBuffer[CURL_ERROR_SIZE] = {};
    const std::string url = directoryUrl(m_profile, path);

    setCurlOption(handle.get(), CURLOPT_URL, url.c_str());
    if (!m_profile.username.empty())
    {
        setCurlOption(handle.get(), CURLOPT_USERNAME, m_profile.username.c_str());
        setCurlOption(handle.get(), CURLOPT_PASSWORD, m_profile.password.c_str());
    }
    setCurlLongOption(handle.get(), CURLOPT_DIRLISTONLY, 1L);
    setCurlLongOption(handle.get(), CURLOPT_FOLLOWLOCATION, 1L);
    setCurlLongOption(handle.get(), CURLOPT_CONNECTTIMEOUT, 15L);
    setCurlLongOption(handle.get(), CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, writeStringCallback);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &listing);

    if (m_profile.protocol == RemoteProtocol::Ftps)
    {
        setCurlLongOption(handle.get(), CURLOPT_USE_SSL, CURLUSESSL_ALL);
    }

    const CURLcode code = curl_easy_perform(handle.get());
    if (code != CURLE_OK)
    {
        const std::string detail = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(code);
        throw std::runtime_error("libcurl directory listing failed: " + detail);
    }

    return parseNameList(listing, path);
}
