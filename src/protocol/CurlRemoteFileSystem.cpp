#include "protocol/CurlRemoteFileSystem.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#include <curl/curl.h>

#ifdef _WIN32
#include <windows.h>
#endif

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

struct CurlStringDeleter
{
    void operator()(char *value) const
    {
        if (value != nullptr)
        {
            curl_free(value);
        }
    }
};

using CurlString = std::unique_ptr<char, CurlStringDeleter>;

struct CurlSlistDeleter
{
    void operator()(curl_slist *list) const
    {
        if (list != nullptr)
        {
            curl_slist_free_all(list);
        }
    }
};

using CurlSlist = std::unique_ptr<curl_slist, CurlSlistDeleter>;

void ensureCurlInitialized()
{
    static std::once_flag initFlag;
    std::call_once(initFlag, []() {
        const CURLcode code = curl_global_init(CURL_GLOBAL_DEFAULT);
        if (code != CURLE_OK)
        {
            throw std::runtime_error(curl_easy_strerror(code));
        }
    });
}

#ifdef _WIN32
/**
 * @brief Converts a UTF-8 string into a Windows wide string.
 * @param value UTF-8 encoded text.
 * @return Wide string suitable for Win32 file APIs.
 */
std::wstring utf8ToWide(const std::string &value)
{
    if (value.empty())
    {
        return {};
    }

    const int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (size <= 0)
    {
        throw std::runtime_error("failed to convert UTF-8 path to wide string");
    }

    std::wstring wideValue(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wideValue.data(), size);
    if (!wideValue.empty() && wideValue.back() == L'\0')
    {
        wideValue.pop_back();
    }
    return wideValue;
}
#endif

/**
 * @brief Opens a local file path using UTF-8 path semantics on every platform.
 * @param path UTF-8 encoded local file path.
 * @param mode Standard C file open mode.
 * @return Open FILE pointer, or nullptr when the file cannot be opened.
 */
FILE *openLocalFile(const std::string &path, const char *mode)
{
#ifdef _WIN32
    const std::wstring widePath = utf8ToWide(path);
    const std::wstring wideMode = utf8ToWide(mode);
    return _wfopen(widePath.c_str(), wideMode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

/**
 * @brief Converts a UTF-8 local path string into a filesystem path.
 * @param path UTF-8 encoded local file path.
 * @return std::filesystem path preserving non-ASCII characters.
 */
std::filesystem::path localFilesystemPath(const std::string &path)
{
    return std::filesystem::u8path(path);
}

/**
 * @brief Creates an empty local temporary file used to emulate remote touch.
 * @return Path to an empty temporary file.
 */
std::filesystem::path createEmptyTemporaryFile()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    const std::filesystem::path path = std::filesystem::temp_directory_path()
        / ("dirbridge-empty-" + std::to_string(millis) + ".tmp");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        throw std::runtime_error("failed to create local empty temporary file");
    }
    return path;
}

size_t writeStringCallback(char *data, size_t size, size_t count, void *userData)
{
    const size_t bytes = size * count;
    auto *buffer = static_cast<std::string *>(userData);
    buffer->append(data, bytes);
    return bytes;
}

size_t writeFileCallback(char *data, size_t size, size_t count, void *userData)
{
    return std::fwrite(data, size, count, static_cast<FILE *>(userData));
}

size_t readFileCallback(char *buffer, size_t size, size_t count, void *userData)
{
    return std::fread(buffer, size, count, static_cast<FILE *>(userData));
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

std::string ftpCommandPath(const std::string &path)
{
    std::string normalized = normalizeRemotePath(path);
    while (!normalized.empty() && normalized.front() == '/')
    {
        normalized.erase(normalized.begin());
    }
    return normalized.empty() ? "." : normalized;
}

std::string remoteParentPath(const std::string &path)
{
    std::string normalized = normalizeRemotePath(path);
    while (normalized.size() > 1 && normalized.back() == '/')
    {
        normalized.pop_back();
    }

    const std::size_t slashIndex = normalized.find_last_of('/');
    if (slashIndex == 0 || slashIndex == std::string::npos)
    {
        return "/";
    }
    return normalized.substr(0, slashIndex);
}

std::string remoteBaseName(const std::string &path)
{
    std::string normalized = normalizeRemotePath(path);
    while (normalized.size() > 1 && normalized.back() == '/')
    {
        normalized.pop_back();
    }

    const std::size_t slashIndex = normalized.find_last_of('/');
    return slashIndex == std::string::npos ? normalized : normalized.substr(slashIndex + 1);
}

std::string escapeUrlPath(CURL *handle, const std::string &path)
{
    const std::string normalized = normalizeRemotePath(path);
    std::ostringstream stream;
    std::string segment;
    for (const char character : normalized)
    {
        if (character == '/')
        {
            if (!segment.empty())
            {
                CurlString escaped(curl_easy_escape(handle, segment.c_str(), static_cast<int>(segment.size())));
                if (!escaped)
                {
                    throw std::runtime_error("failed to escape remote URL path");
                }
                stream << escaped.get();
                segment.clear();
            }
            stream << '/';
        }
        else
        {
            segment.push_back(character);
        }
    }

    if (!segment.empty())
    {
        CurlString escaped(curl_easy_escape(handle, segment.c_str(), static_cast<int>(segment.size())));
        if (!escaped)
        {
            throw std::runtime_error("failed to escape remote URL path");
        }
        stream << escaped.get();
    }

    return stream.str();
}

std::string directoryUrl(CURL *handle, const SiteProfile &profile, const std::string &path)
{
    std::ostringstream stream;
    stream << toString(profile.protocol) << "://" << profile.host << ':' << profile.port;
    stream << escapeUrlPath(handle, path);
    if (stream.str().back() != '/')
    {
        stream << '/';
    }
    return stream.str();
}

std::string fileUrl(CURL *handle, const SiteProfile &profile, const std::string &path)
{
    std::ostringstream stream;
    stream << toString(profile.protocol) << "://" << profile.host << ':' << profile.port;
    stream << escapeUrlPath(handle, path);
    return stream.str();
}

std::string rootUrl(const SiteProfile &profile)
{
    std::ostringstream stream;
    stream << toString(profile.protocol) << "://" << profile.host << ':' << profile.port << '/';
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
        if (name.empty() || name == "." || name == ".." || name.rfind("total ", 0) == 0)
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

std::vector<std::string> splitWhitespace(const std::string &value, int maxParts = -1)
{
    std::vector<std::string> parts;
    std::istringstream stream(value);
    std::string part;
    while ((maxParts <= 0 || static_cast<int>(parts.size()) < maxParts - 1) && stream >> part)
    {
        parts.push_back(part);
    }

    if (maxParts > 0 && static_cast<int>(parts.size()) == maxParts - 1)
    {
        std::string remainder;
        std::getline(stream, remainder);
        remainder = trimCopy(remainder);
        if (!remainder.empty())
        {
            parts.push_back(remainder);
        }
    }
    return parts;
}

std::string stripSymlinkTarget(const std::string &name)
{
    const std::string marker = " -> ";
    const std::size_t markerIndex = name.find(marker);
    return markerIndex == std::string::npos ? name : name.substr(0, markerIndex);
}

bool parseUnixListLine(const std::string &line, const std::string &directory, FileItem &item)
{
    const std::string trimmedLine = trimCopy(line);
    if (trimmedLine.size() < 11)
    {
        return false;
    }

    const char typeCharacter = trimmedLine.front();
    if (typeCharacter != 'd' && typeCharacter != '-' && typeCharacter != 'l')
    {
        return false;
    }

    const std::vector<std::string> parts = splitWhitespace(trimmedLine, 9);
    if (parts.size() < 9)
    {
        return false;
    }

    std::string name = stripSymlinkTarget(parts.at(8));
    if (name.empty() || name == "." || name == "..")
    {
        return false;
    }

    item.name = name;
    item.path = joinRemotePath(directory, name);
    item.type = typeCharacter == 'd'
        ? FileItemType::Directory
        : typeCharacter == 'l' ? FileItemType::Symlink : FileItemType::File;
    item.size = std::strtoll(parts.at(4).c_str(), nullptr, 10);
    item.modifiedTime = parts.at(5) + " " + parts.at(6) + " " + parts.at(7);
    item.permissions = parts.at(0);
    item.owner = parts.at(2);
    return true;
}

bool isUnixListLine(const std::string &line)
{
    const std::string trimmedLine = trimCopy(line);
    if (trimmedLine.size() < 11)
    {
        return false;
    }

    const char typeCharacter = trimmedLine.front();
    if (typeCharacter != 'd' && typeCharacter != '-' && typeCharacter != 'l')
    {
        return false;
    }

    return splitWhitespace(trimmedLine, 9).size() >= 9;
}

std::vector<FileItem> parseDetailedList(const std::string &listing, const std::string &directory)
{
    std::vector<FileItem> items;
    std::istringstream stream(listing);
    std::string line;

    while (std::getline(stream, line))
    {
        FileItem item;
        if (parseUnixListLine(line, directory, item))
        {
            items.push_back(std::move(item));
        }
    }

    return items;
}

bool containsUnixListLines(const std::string &listing)
{
    std::istringstream stream(listing);
    std::string line;
    while (std::getline(stream, line))
    {
        if (isUnixListLine(line))
        {
            return true;
        }
    }

    return false;
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

void applyProfileOptions(CURL *handle, const SiteProfile &profile)
{
    if (!profile.username.empty())
    {
        setCurlOption(handle, CURLOPT_USERNAME, profile.username.c_str());
        setCurlOption(handle, CURLOPT_PASSWORD, profile.password.c_str());
    }
    setCurlLongOption(handle, CURLOPT_FOLLOWLOCATION, 1L);
    setCurlLongOption(handle, CURLOPT_CONNECTTIMEOUT, 15L);
    setCurlLongOption(handle, CURLOPT_TIMEOUT, 300L);
    if (profile.protocol == RemoteProtocol::Ftps)
    {
        setCurlLongOption(handle, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    }
}

RemoteOperationResult performQuoteAtUrl(const SiteProfile &profile, const std::string &url, const std::vector<std::string> &commands)
{
    ensureCurlInitialized();
    CurlHandle handle(curl_easy_init());
    if (!handle)
    {
        return {false, "failed to initialize libcurl easy handle"};
    }

    char errorBuffer[CURL_ERROR_SIZE] = {};
    try
    {
        setCurlOption(handle.get(), CURLOPT_URL, url.c_str());
        applyProfileOptions(handle.get(), profile);
        setCurlLongOption(handle.get(), CURLOPT_NOBODY, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, errorBuffer);

        curl_slist *rawList = nullptr;
        for (const std::string &command : commands)
        {
            rawList = curl_slist_append(rawList, command.c_str());
        }
        CurlSlist commandList(rawList);
        curl_easy_setopt(handle.get(), CURLOPT_QUOTE, commandList.get());

        const CURLcode code = curl_easy_perform(handle.get());
        if (code != CURLE_OK)
        {
            const std::string detail = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(code);
            return {false, detail};
        }
    }
    catch (const std::exception &error)
    {
        return {false, error.what()};
    }

    return {true, "remote command succeeded"};
}

RemoteOperationResult performQuote(const SiteProfile &profile, const std::vector<std::string> &commands)
{
    return performQuoteAtUrl(profile, rootUrl(profile), commands);
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

    ensureCurlInitialized();
    CurlHandle handle(curl_easy_init());
    if (!handle)
    {
        throw std::runtime_error("failed to initialize libcurl easy handle");
    }

    std::string listing;
    char errorBuffer[CURL_ERROR_SIZE] = {};
    const std::string url = directoryUrl(handle.get(), m_profile, path);

    setCurlOption(handle.get(), CURLOPT_URL, url.c_str());
    applyProfileOptions(handle.get(), m_profile);
    curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, writeStringCallback);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &listing);

    const CURLcode code = curl_easy_perform(handle.get());
    if (code != CURLE_OK)
    {
        const std::string detail = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(code);
        throw std::runtime_error("libcurl directory listing failed: " + detail);
    }

    const bool detailedListing = containsUnixListLines(listing);
    std::vector<FileItem> items = parseDetailedList(listing, path);
    if (!detailedListing && items.empty() && !trimCopy(listing).empty())
    {
        items = parseNameList(listing, path);
    }

    return items;
}

RemoteOperationResult CurlRemoteFileSystem::createDirectory(const std::string &path)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedPath = normalizeRemotePath(path);
    if (m_profile.protocol == RemoteProtocol::Sftp)
    {
        return performQuote(m_profile, {"mkdir " + normalizedPath});
    }

    return performQuoteAtUrl(m_profile, rootUrl(m_profile), {"MKD " + ftpCommandPath(normalizedPath)});
}

RemoteOperationResult CurlRemoteFileSystem::createFile(const std::string &path)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedPath = normalizeRemotePath(path);
    const std::string parentPath = remoteParentPath(normalizedPath);
    const std::string fileName = remoteBaseName(normalizedPath);
    if (fileName.empty())
    {
        return {false, "remote file name is empty"};
    }

    try
    {
        const std::vector<FileItem> siblings = listDirectory(parentPath);
        const auto existing = std::find_if(siblings.begin(), siblings.end(), [&fileName](const FileItem &item) {
            return item.name == fileName;
        });
        if (existing != siblings.end())
        {
            return {false, "remote item already exists"};
        }
    }
    catch (const std::exception &error)
    {
        return {false, std::string("remote parent directory check failed: ") + error.what()};
    }

    std::filesystem::path temporaryPath;
    try
    {
        temporaryPath = createEmptyTemporaryFile();
        const RemoteOperationResult result = uploadFile(temporaryPath.u8string(), normalizedPath);
        std::error_code removeError;
        std::filesystem::remove(temporaryPath, removeError);
        if (!result.success)
        {
            return {false, "create file failed: " + result.message};
        }
    }
    catch (const std::exception &error)
    {
        if (!temporaryPath.empty())
        {
            std::error_code removeError;
            std::filesystem::remove(temporaryPath, removeError);
        }
        return {false, error.what()};
    }

    return {true, "file created"};
}

RemoteOperationResult CurlRemoteFileSystem::remove(const std::string &path)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedPath = normalizeRemotePath(path);
    RemoteOperationResult fileResult;
    RemoteOperationResult directoryResult;
    if (m_profile.protocol == RemoteProtocol::Sftp)
    {
        fileResult = performQuote(m_profile, {"rm " + normalizedPath});
        if (fileResult.success)
        {
            return fileResult;
        }
        directoryResult = performQuote(m_profile, {"rmdir " + normalizedPath});
    }
    else
    {
        fileResult = performQuoteAtUrl(m_profile, rootUrl(m_profile), {"DELE " + ftpCommandPath(normalizedPath)});
        if (fileResult.success)
        {
            return fileResult;
        }
        directoryResult = performQuoteAtUrl(m_profile, rootUrl(m_profile), {"RMD " + ftpCommandPath(normalizedPath)});
    }

    if (directoryResult.success)
    {
        return directoryResult;
    }

    return {false, "remove failed: " + fileResult.message + "; " + directoryResult.message};
}

RemoteOperationResult CurlRemoteFileSystem::rename(const std::string &sourcePath, const std::string &targetPath)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedSourcePath = normalizeRemotePath(sourcePath);
    const std::string normalizedTargetPath = normalizeRemotePath(targetPath);
    if (m_profile.protocol == RemoteProtocol::Sftp)
    {
        return performQuote(m_profile, {"rename " + normalizedSourcePath + " " + normalizedTargetPath});
    }

    return performQuoteAtUrl(
        m_profile,
        rootUrl(m_profile),
        {"RNFR " + ftpCommandPath(normalizedSourcePath), "RNTO " + ftpCommandPath(normalizedTargetPath)});
}

RemoteOperationResult CurlRemoteFileSystem::uploadFile(const std::string &localPath, const std::string &remotePath)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    ensureCurlInitialized();
    FILE *file = openLocalFile(localPath, "rb");
    if (file == nullptr)
    {
        return {false, "failed to open local file for upload"};
    }

    CurlHandle handle(curl_easy_init());
    if (!handle)
    {
        std::fclose(file);
        return {false, "failed to initialize libcurl easy handle"};
    }

    char errorBuffer[CURL_ERROR_SIZE] = {};
    const std::string url = fileUrl(handle.get(), m_profile, remotePath);
    try
    {
        setCurlOption(handle.get(), CURLOPT_URL, url.c_str());
        applyProfileOptions(handle.get(), m_profile);
        setCurlLongOption(handle.get(), CURLOPT_UPLOAD, 1L);
        curl_easy_setopt(handle.get(), CURLOPT_READFUNCTION, readFileCallback);
        curl_easy_setopt(handle.get(), CURLOPT_READDATA, file);
        curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, errorBuffer);

        const std::uintmax_t size = std::filesystem::file_size(localFilesystemPath(localPath));
        curl_easy_setopt(handle.get(), CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));

        const CURLcode code = curl_easy_perform(handle.get());
        if (code != CURLE_OK)
        {
            handle.reset();
            std::fclose(file);
            const std::string detail = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(code);
            return {false, detail};
        }
        handle.reset();
        std::fclose(file);
    }
    catch (const std::exception &error)
    {
        handle.reset();
        std::fclose(file);
        return {false, error.what()};
    }

    return {true, "upload succeeded"};
}

RemoteOperationResult CurlRemoteFileSystem::downloadFile(const std::string &remotePath, const std::string &localPath)
{
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    ensureCurlInitialized();
    const std::filesystem::path targetPath = localFilesystemPath(localPath);
    if (targetPath.has_parent_path())
    {
        std::filesystem::create_directories(targetPath.parent_path());
    }

    FILE *file = openLocalFile(localPath, "wb");
    if (file == nullptr)
    {
        return {false, "failed to open local file for download: " + localPath};
    }

    CurlHandle handle(curl_easy_init());
    if (!handle)
    {
        std::fclose(file);
        return {false, "failed to initialize libcurl easy handle"};
    }

    char errorBuffer[CURL_ERROR_SIZE] = {};
    const std::string url = fileUrl(handle.get(), m_profile, remotePath);
    try
    {
        setCurlOption(handle.get(), CURLOPT_URL, url.c_str());
        applyProfileOptions(handle.get(), m_profile);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, writeFileCallback);
        curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, file);
        curl_easy_setopt(handle.get(), CURLOPT_ERRORBUFFER, errorBuffer);

        const CURLcode code = curl_easy_perform(handle.get());
        if (code != CURLE_OK)
        {
            handle.reset();
            std::fclose(file);
            std::filesystem::remove(targetPath);
            const std::string detail = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(code);
            return {false, detail};
        }
        handle.reset();
        std::fclose(file);
    }
    catch (const std::exception &error)
    {
        handle.reset();
        std::fclose(file);
        std::filesystem::remove(targetPath);
        return {false, error.what()};
    }

    return {true, "download succeeded"};
}
