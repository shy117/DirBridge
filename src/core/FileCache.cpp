#include "core/FileCache.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>

namespace
{
std::string hashToHex(const std::string &value)
{
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : value)
    {
        hash ^= character;
        hash *= 1099511628211ULL;
    }

    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::setw(16) << hash;
    return stream.str();
}

std::string safeExtensionForPath(const std::string &remotePath)
{
    const std::size_t slash = remotePath.find_last_of('/');
    const std::string filename = slash == std::string::npos ? remotePath : remotePath.substr(slash + 1);
    const std::size_t dot = filename.find_last_of('.');
    if (dot == std::string::npos || dot == 0 || filename.size() - dot > 17)
    {
        return {};
    }

    const std::string extension = filename.substr(dot);
    const bool valid = std::all_of(extension.begin() + 1, extension.end(), [](unsigned char character) {
        return (character >= 'a' && character <= 'z')
            || (character >= 'A' && character <= 'Z')
            || (character >= '0' && character <= '9');
    });
    return valid ? extension : std::string();
}

bool isWindowsReservedName(const std::string &filename)
{
    const std::size_t dot = filename.find('.');
    std::string stem = filename.substr(0, dot);
    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });

    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL")
    {
        return true;
    }
    if (stem.size() == 4 && (stem.rfind("COM", 0) == 0 || stem.rfind("LPT", 0) == 0))
    {
        return stem.back() >= '1' && stem.back() <= '9';
    }
    return false;
}

std::string safeWorkingFilename(const std::string &remotePath)
{
    const std::size_t slash = remotePath.find_last_of('/');
    std::string filename = slash == std::string::npos ? remotePath : remotePath.substr(slash + 1);
    for (char &character : filename)
    {
        const unsigned char value = static_cast<unsigned char>(character);
        if (value <= 31
            || character == '<' || character == '>' || character == ':' || character == '"'
            || character == '/' || character == '\\' || character == '|' || character == '?' || character == '*')
        {
            character = '_';
        }
    }
    while (!filename.empty() && (filename.back() == '.' || filename.back() == ' '))
    {
        filename.pop_back();
    }

    if (filename.empty() || filename == "." || filename == ".." || isWindowsReservedName(filename))
    {
        return "content" + safeExtensionForPath(remotePath);
    }
    return filename;
}

FileCacheResult failure(const std::string &message)
{
    return {false, message};
}
}

FileCache::FileCache(std::filesystem::path rootDirectory)
    : m_rootDirectory(std::move(rootDirectory))
{
}

const std::filesystem::path &FileCache::rootDirectory() const
{
    return m_rootDirectory;
}

FileCacheEntry FileCache::createEntry(const std::string &sessionId, const std::string &remotePath) const
{
    const std::string documentId = makeDocumentId(sessionId, remotePath);
    const std::string sessionKey = hashToHex(sessionId);
    const std::filesystem::path directory = m_rootDirectory / sessionKey / documentId;

    FileCacheEntry entry;
    entry.documentId = documentId;
    entry.directory = directory;
    entry.workingFilePath = directory / safeWorkingFilename(remotePath);
    entry.downloadTemporaryPath = directory / "download.tmp";
    entry.snapshotsDirectory = directory / "snapshots";
    return entry;
}

FileCacheResult FileCache::prepareEntry(const FileCacheEntry &entry) const
{
    if (!ownsEntryDirectory(entry.directory))
    {
        return failure("cache entry is outside the configured cache root");
    }

    std::error_code error;
    std::filesystem::create_directories(entry.snapshotsDirectory, error);
    if (error)
    {
        return failure("unable to create editor cache directory: " + error.message());
    }
    return {true, {}};
}

FileCacheResult FileCache::commitDownloadedFile(const FileCacheEntry &entry) const
{
    if (!ownsEntryDirectory(entry.directory))
    {
        return failure("cache entry is outside the configured cache root");
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(entry.downloadTemporaryPath, error) || error)
    {
        return failure("downloaded temporary cache file is missing");
    }
    if (std::filesystem::exists(entry.workingFilePath, error) || error)
    {
        if (error)
        {
            return failure("unable to inspect existing editor cache working file: " + error.message());
        }
        const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        const std::string recoveryBaseName = "recovered-" + std::to_string(timestamp);
        const std::string extension = entry.workingFilePath.extension().string();
        std::filesystem::path recoveryPath = entry.snapshotsDirectory / (recoveryBaseName + extension);
        for (int suffix = 1; std::filesystem::exists(recoveryPath, error) && !error; ++suffix)
        {
            recoveryPath = entry.snapshotsDirectory
                / (recoveryBaseName + "-" + std::to_string(suffix) + extension);
        }
        if (error)
        {
            return failure("unable to select editor cache recovery path: " + error.message());
        }
        std::filesystem::rename(entry.workingFilePath, recoveryPath, error);
        if (error)
        {
            return failure("unable to preserve existing editor cache working file: " + error.message());
        }
    }

    std::filesystem::rename(entry.downloadTemporaryPath, entry.workingFilePath, error);
    if (error)
    {
        return failure("unable to commit downloaded cache file: " + error.message());
    }
    return {true, {}};
}

FileCacheResult FileCache::createUploadSnapshot(const FileCacheEntry &entry,
                                                 std::uint64_t version,
                                                 std::filesystem::path &snapshotPath) const
{
    if (!ownsEntryDirectory(entry.directory))
    {
        return failure("cache entry is outside the configured cache root");
    }

    std::error_code error;
    if (!std::filesystem::is_regular_file(entry.workingFilePath, error) || error)
    {
        return failure("editor cache working file is missing");
    }

    snapshotPath = entry.snapshotsDirectory
        / ("upload-" + std::to_string(version) + entry.workingFilePath.extension().string());
    if (std::filesystem::exists(snapshotPath, error) && !error)
    {
        std::filesystem::remove(snapshotPath, error);
    }
    if (error)
    {
        return failure("unable to replace editor upload snapshot: " + error.message());
    }

    std::filesystem::copy_file(entry.workingFilePath, snapshotPath, std::filesystem::copy_options::none, error);
    if (error)
    {
        return failure("unable to create editor upload snapshot: " + error.message());
    }
    return {true, {}};
}

FileCacheResult FileCache::removeEntry(const FileCacheEntry &entry) const
{
    if (!ownsEntryDirectory(entry.directory))
    {
        return failure("cache entry is outside the configured cache root");
    }

    std::error_code error;
    std::filesystem::remove_all(entry.directory, error);
    if (error)
    {
        return failure("unable to remove editor cache directory: " + error.message());
    }
    return {true, {}};
}

std::string FileCache::makeDocumentId(const std::string &sessionId, const std::string &remotePath)
{
    return hashToHex(sessionId + "\n" + remotePath);
}

bool FileCache::ownsEntryDirectory(const std::filesystem::path &directory) const
{
    const std::filesystem::path relative = directory.lexically_relative(m_rootDirectory);
    if (relative.empty() || relative.is_absolute())
    {
        return false;
    }
    return std::none_of(relative.begin(), relative.end(), [](const std::filesystem::path &component) {
        return component == "..";
    });
}
