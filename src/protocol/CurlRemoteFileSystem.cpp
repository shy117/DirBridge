#include "protocol/CurlRemoteFileSystem.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
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
 * @brief 将 UTF-8 字符串转换为 Windows 宽字符字符串。
 * @param value UTF-8 编码文本。
 * @return 适合 Win32 文件 API 使用的宽字符串。
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
 * @brief 在各平台上按 UTF-8 路径语义打开本地文件。
 * @param path UTF-8 编码的本地文件路径。
 * @param mode 标准 C 文件打开模式。
 * @return 打开的 FILE 指针；无法打开时返回 nullptr。
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
 * @brief 将 UTF-8 本地路径字符串转换为文件系统路径对象。
 * @param path UTF-8 编码的本地文件路径。
 * @return 可保留非 ASCII 字符的 `std::filesystem::path`。
 */
std::filesystem::path localFilesystemPath(const std::string &path)
{
    return std::filesystem::u8path(path);
}

/**
 * @brief 创建一个用于模拟远程 touch 的本地空临时文件。
 * @return 指向空临时文件的路径。
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

int transferProgressCallback(void *userData, curl_off_t downloadTotal, curl_off_t downloadNow, curl_off_t uploadTotal, curl_off_t uploadNow)
{
    auto *progress = static_cast<TransferProgressCallback *>(userData);
    if (progress == nullptr || !(*progress))
    {
        return 0;
    }

    const curl_off_t total = uploadTotal > 0 ? uploadTotal : downloadTotal;
    const curl_off_t now = uploadNow > 0 ? uploadNow : downloadNow;
    return (*progress)(static_cast<std::int64_t>(now), static_cast<std::int64_t>(total)) ? 0 : 1;
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

std::vector<std::string> remotePathSegments(const std::string &path)
{
    const std::string normalized = normalizeRemotePath(path);
    std::vector<std::string> segments;
    std::string segment;
    for (const char character : normalized)
    {
        if (character == '/')
        {
            if (!segment.empty())
            {
                segments.push_back(segment);
                segment.clear();
            }
            continue;
        }
        segment.push_back(character);
    }
    if (!segment.empty())
    {
        segments.push_back(segment);
    }
    return segments;
}

std::string commonRemoteDirectory(const std::string &leftPath, const std::string &rightPath)
{
    const std::vector<std::string> leftSegments = remotePathSegments(leftPath);
    const std::vector<std::string> rightSegments = remotePathSegments(rightPath);
    const std::size_t limit = std::min(leftSegments.size(), rightSegments.size());

    std::ostringstream commonPath;
    for (std::size_t index = 0; index < limit && leftSegments[index] == rightSegments[index]; ++index)
    {
        commonPath << '/' << leftSegments[index];
    }
    return commonPath.str().empty() ? "/" : commonPath.str();
}

std::string relativeRemotePath(const std::string &baseDirectory, const std::string &path)
{
    const std::vector<std::string> baseSegments = remotePathSegments(baseDirectory);
    const std::vector<std::string> pathSegments = remotePathSegments(path);
    if (baseSegments.size() > pathSegments.size()
        || !std::equal(baseSegments.begin(), baseSegments.end(), pathSegments.begin()))
    {
        throw std::invalid_argument("remote path is outside the FTP command directory");
    }

    std::ostringstream relativePath;
    for (std::size_t index = baseSegments.size(); index < pathSegments.size(); ++index)
    {
        if (index > baseSegments.size())
        {
            relativePath << '/';
        }
        relativePath << pathSegments[index];
    }
    return relativePath.str().empty() ? "." : relativePath.str();
}

std::string ftpCommandSummary(const std::vector<std::string> &commands)
{
    std::ostringstream summary;
    for (const std::string &command : commands)
    {
        if (summary.tellp() > 0)
        {
            summary << '/';
        }
        if (command.compare(0, 10, "SITE CHMOD") == 0)
        {
            summary << "SITE CHMOD";
            continue;
        }
        const std::size_t spaceIndex = command.find(' ');
        summary << command.substr(0, spaceIndex);
    }
    return summary.str().empty() ? "command" : summary.str();
}

std::string sftpCommandPath(const std::string &path)
{
    const std::string normalized = normalizeRemotePath(path);
    std::string escaped;
    escaped.reserve(normalized.size() + 2);
    escaped.push_back('"');
    for (const char character : normalized)
    {
        if (character == '\\' || character == '"')
        {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    escaped.push_back('"');
    return escaped;
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
    setCurlLongOption(handle, CURLOPT_NOSIGNAL, 1L);
    setCurlLongOption(handle, CURLOPT_CONNECTTIMEOUT, 15L);
    setCurlLongOption(handle, CURLOPT_TIMEOUT, 300L);
    if (profile.protocol == RemoteProtocol::Ftps)
    {
        setCurlLongOption(handle, CURLOPT_USE_SSL, CURLUSESSL_ALL);
    }
}

}

CurlRemoteFileSystem::~CurlRemoteFileSystem()
{
    disconnect();
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

    std::scoped_lock lock(m_directoryHandleMutex, m_transferHandleMutex);
    if (m_directoryHandle != nullptr)
    {
        curl_easy_cleanup(m_directoryHandle);
        m_directoryHandle = nullptr;
    }
    if (m_transferHandle != nullptr)
    {
        curl_easy_cleanup(m_transferHandle);
        m_transferHandle = nullptr;
    }
    m_profile = profile;
    m_connected = true;
    return {true, "connection profile accepted"};
}

void CurlRemoteFileSystem::disconnect()
{
    std::scoped_lock lock(m_directoryHandleMutex, m_transferHandleMutex);
    m_connected = false;
    if (m_directoryHandle != nullptr)
    {
        curl_easy_cleanup(m_directoryHandle);
        m_directoryHandle = nullptr;
    }
    if (m_transferHandle != nullptr)
    {
        curl_easy_cleanup(m_transferHandle);
        m_transferHandle = nullptr;
    }
}

bool CurlRemoteFileSystem::isConnected() const
{
    std::lock_guard<std::mutex> lock(m_directoryHandleMutex);
    return m_connected;
}

CURL *CurlRemoteFileSystem::prepareHandleLocked(CURL *&handle)
{
    ensureCurlInitialized();
    if (handle == nullptr)
    {
        handle = curl_easy_init();
    }
    else
    {
        curl_easy_reset(handle);
    }
    if (handle == nullptr)
    {
        throw std::runtime_error("failed to initialize libcurl easy handle");
    }
    return handle;
}

RemoteOperationResult CurlRemoteFileSystem::performQuoteAtUrlLocked(
    const std::string &url,
    const std::vector<std::string> &commands)
{
    char errorBuffer[CURL_ERROR_SIZE] = {};
    try
    {
        CURL *handle = prepareHandleLocked(m_directoryHandle);
        setCurlOption(handle, CURLOPT_URL, url.c_str());
        applyProfileOptions(handle, m_profile);
        setCurlLongOption(handle, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);

        curl_slist *rawList = nullptr;
        for (const std::string &command : commands)
        {
            rawList = curl_slist_append(rawList, command.c_str());
        }
        CurlSlist commandList(rawList);
        curl_easy_setopt(handle, CURLOPT_QUOTE, commandList.get());

        const CURLcode code = curl_easy_perform(handle);
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

RemoteOperationResult CurlRemoteFileSystem::performFtpCommandsInDirectoryLocked(
    const std::string &directoryPath,
    const std::vector<std::string> &commands)
{
    // PREQUOTE runs after libcurl has changed into the URL directory. FTP mutations must not
    // use QUOTE with relative paths because QUOTE runs before that directory change.
    char errorBuffer[CURL_ERROR_SIZE] = {};
    try
    {
        CURL *handle = prepareHandleLocked(m_directoryHandle);
        const std::string url = directoryUrl(handle, m_profile, directoryPath);
        setCurlOption(handle, CURLOPT_URL, url.c_str());
        applyProfileOptions(handle, m_profile);
        setCurlLongOption(handle, CURLOPT_NOBODY, 1L);
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);

        curl_slist *rawList = nullptr;
        for (const std::string &command : commands)
        {
            rawList = curl_slist_append(rawList, command.c_str());
        }
        CurlSlist commandList(rawList);
        curl_easy_setopt(handle, CURLOPT_PREQUOTE, commandList.get());

        const CURLcode code = curl_easy_perform(handle);
        if (code != CURLE_OK)
        {
            const std::string detail = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(code);
            long responseCode = 0;
            curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &responseCode);

            std::ostringstream message;
            message << "FTP " << ftpCommandSummary(commands) << " failed";
            if (responseCode > 0)
            {
                message << " (response " << responseCode << ')';
            }
            message << ": " << detail;
            return {false, message.str()};
        }
    }
    catch (const std::exception &error)
    {
        return {false, error.what()};
    }

    return {true, "remote command succeeded"};
}

std::vector<FileItem> CurlRemoteFileSystem::listDirectory(const std::string &path)
{
    std::lock_guard<std::mutex> lock(m_directoryHandleMutex);
    if (!m_connected)
    {
        throw std::runtime_error("remote session is not connected");
    }

    CURL *handle = prepareHandleLocked(m_directoryHandle);

    std::string listing;
    char errorBuffer[CURL_ERROR_SIZE] = {};
    const std::string url = directoryUrl(handle, m_profile, path);

    setCurlOption(handle, CURLOPT_URL, url.c_str());
    applyProfileOptions(handle, m_profile);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeStringCallback);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &listing);

    const CURLcode code = curl_easy_perform(handle);
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
    std::lock_guard<std::mutex> lock(m_directoryHandleMutex);
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedPath = normalizeRemotePath(path);
    if (m_profile.protocol == RemoteProtocol::Sftp)
    {
        return performQuoteAtUrlLocked(rootUrl(m_profile), {"mkdir " + sftpCommandPath(normalizedPath)});
    }

    const std::string directoryName = remoteBaseName(normalizedPath);
    if (directoryName.empty())
    {
        return {false, "remote directory name is empty"};
    }
    return performFtpCommandsInDirectoryLocked(
        remoteParentPath(normalizedPath),
        {"MKD " + directoryName});
}

RemoteOperationResult CurlRemoteFileSystem::createFile(const std::string &path)
{
    if (!isConnected())
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
    const RemoteOperationResult fileResult = removeFile(path);
    if (fileResult.success)
    {
        return fileResult;
    }
    const RemoteOperationResult directoryResult = removeDirectory(path);
    if (directoryResult.success)
    {
        return directoryResult;
    }
    return {false, "remove failed: " + fileResult.message + "; " + directoryResult.message};
}

RemoteOperationResult CurlRemoteFileSystem::removeFile(const std::string &path)
{
    std::lock_guard<std::mutex> lock(m_directoryHandleMutex);
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }
    const std::string normalizedPath = normalizeRemotePath(path);
    if (m_profile.protocol == RemoteProtocol::Sftp)
    {
        return performQuoteAtUrlLocked(rootUrl(m_profile), {"rm " + sftpCommandPath(normalizedPath)});
    }

    return performFtpCommandsInDirectoryLocked(
        remoteParentPath(normalizedPath),
        {"DELE " + remoteBaseName(normalizedPath)});
}

RemoteOperationResult CurlRemoteFileSystem::removeDirectory(const std::string &path)
{
    std::lock_guard<std::mutex> lock(m_directoryHandleMutex);
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedPath = normalizeRemotePath(path);
    if (m_profile.protocol == RemoteProtocol::Sftp)
    {
        return performQuoteAtUrlLocked(rootUrl(m_profile), {"rmdir " + sftpCommandPath(normalizedPath)});
    }
    return performFtpCommandsInDirectoryLocked(
        remoteParentPath(normalizedPath),
        {"RMD " + remoteBaseName(normalizedPath)});
}

RemoteOperationResult CurlRemoteFileSystem::rename(const std::string &sourcePath, const std::string &targetPath)
{
    std::lock_guard<std::mutex> lock(m_directoryHandleMutex);
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }

    const std::string normalizedSourcePath = normalizeRemotePath(sourcePath);
    const std::string normalizedTargetPath = normalizeRemotePath(targetPath);
    if (m_profile.protocol == RemoteProtocol::Sftp)
    {
        return performQuoteAtUrlLocked(
            rootUrl(m_profile),
            {"rename " + sftpCommandPath(normalizedSourcePath) + " " + sftpCommandPath(normalizedTargetPath)});
    }

    const std::string commandDirectory = commonRemoteDirectory(
        remoteParentPath(normalizedSourcePath),
        remoteParentPath(normalizedTargetPath));
    return performFtpCommandsInDirectoryLocked(
        commandDirectory,
        {"RNFR " + relativeRemotePath(commandDirectory, normalizedSourcePath),
         "RNTO " + relativeRemotePath(commandDirectory, normalizedTargetPath)});
}

RemoteOperationResult CurlRemoteFileSystem::setPermissions(const std::string &path, int mode)
{
    std::lock_guard<std::mutex> lock(m_directoryHandleMutex);
    if (!m_connected)
    {
        return {false, "remote session is not connected"};
    }
    if (mode < 0 || mode > 0777)
    {
        return {false, "permission mode must be between 000 and 777"};
    }

    std::ostringstream modeText;
    modeText << std::oct << std::setw(3) << std::setfill('0') << mode;
    const std::string normalizedPath = normalizeRemotePath(path);
    if (m_profile.protocol == RemoteProtocol::Sftp)
    {
        return performQuoteAtUrlLocked(rootUrl(m_profile), {"chmod " + modeText.str() + " " + sftpCommandPath(normalizedPath)});
    }

    return performFtpCommandsInDirectoryLocked(
        remoteParentPath(normalizedPath),
        {"SITE CHMOD " + modeText.str() + " "
         + relativeRemotePath(remoteParentPath(normalizedPath), normalizedPath)});
}

RemoteOperationResult CurlRemoteFileSystem::uploadFile(const std::string &localPath, const std::string &remotePath, TransferProgressCallback progress)
{
    std::lock_guard<std::mutex> lock(m_transferHandleMutex);
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

    char errorBuffer[CURL_ERROR_SIZE] = {};
    try
    {
        CURL *handle = prepareHandleLocked(m_transferHandle);
        const std::string url = fileUrl(handle, m_profile, remotePath);
        setCurlOption(handle, CURLOPT_URL, url.c_str());
        applyProfileOptions(handle, m_profile);
        setCurlLongOption(handle, CURLOPT_UPLOAD, 1L);
        setCurlLongOption(handle, CURLOPT_UPLOAD_BUFFERSIZE, 256L * 1024L);
        curl_easy_setopt(handle, CURLOPT_READFUNCTION, readFileCallback);
        curl_easy_setopt(handle, CURLOPT_READDATA, file);
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);
        if (progress)
        {
            curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, transferProgressCallback);
            curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &progress);
        }

        const std::uintmax_t size = std::filesystem::file_size(localFilesystemPath(localPath));
        curl_easy_setopt(handle, CURLOPT_INFILESIZE_LARGE, static_cast<curl_off_t>(size));

        const CURLcode code = curl_easy_perform(handle);
        if (code != CURLE_OK)
        {
            std::fclose(file);
            const std::string detail = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(code);
            return {false, detail};
        }
        std::fclose(file);
    }
    catch (const std::exception &error)
    {
        std::fclose(file);
        return {false, error.what()};
    }

    return {true, "upload succeeded"};
}

RemoteOperationResult CurlRemoteFileSystem::downloadFile(const std::string &remotePath, const std::string &localPath, TransferProgressCallback progress)
{
    std::lock_guard<std::mutex> lock(m_transferHandleMutex);
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

    char errorBuffer[CURL_ERROR_SIZE] = {};
    try
    {
        CURL *handle = prepareHandleLocked(m_transferHandle);
        const std::string url = fileUrl(handle, m_profile, remotePath);
        setCurlOption(handle, CURLOPT_URL, url.c_str());
        applyProfileOptions(handle, m_profile);
        setCurlLongOption(handle, CURLOPT_BUFFERSIZE, 256L * 1024L);
        curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, writeFileCallback);
        curl_easy_setopt(handle, CURLOPT_WRITEDATA, file);
        curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, errorBuffer);
        if (progress)
        {
            curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, transferProgressCallback);
            curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &progress);
        }

        const CURLcode code = curl_easy_perform(handle);
        if (code != CURLE_OK)
        {
            std::fclose(file);
            std::filesystem::remove(targetPath);
            const std::string detail = errorBuffer[0] != '\0' ? errorBuffer : curl_easy_strerror(code);
            return {false, detail};
        }
        std::fclose(file);
    }
    catch (const std::exception &error)
    {
        std::fclose(file);
        std::filesystem::remove(targetPath);
        return {false, error.what()};
    }

    return {true, "download succeeded"};
}
