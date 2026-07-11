#ifndef DIRBRIDGE_CORE_FILECACHE_H
#define DIRBRIDGE_CORE_FILECACHE_H

#include <cstdint>
#include <filesystem>
#include <string>

/**
 * @brief 远程编辑文档在本地缓存中的路径集合。
 */
struct FileCacheEntry
{
    std::string documentId;
    std::filesystem::path directory;
    std::filesystem::path workingFilePath;
    std::filesystem::path downloadTemporaryPath;
    std::filesystem::path snapshotsDirectory;
};

/**
 * @brief 缓存文件操作结果。
 */
struct FileCacheResult
{
    bool success = false;
    std::string message;
};

/**
 * @brief 为远程外部编辑提供安全、隔离的本地缓存。
 *
 * 缓存键只由会话 ID 和远程路径生成；仅经 Windows 文件名规则清理后的远程基名可作为工作副本名称，
 * 远程完整路径不会直接拼接到本地目录。
 */
class FileCache
{
public:
    explicit FileCache(std::filesystem::path rootDirectory);

    const std::filesystem::path &rootDirectory() const;

    FileCacheEntry createEntry(const std::string &sessionId, const std::string &remotePath) const;
    FileCacheResult prepareEntry(const FileCacheEntry &entry) const;
    FileCacheResult commitDownloadedFile(const FileCacheEntry &entry) const;
    FileCacheResult createUploadSnapshot(const FileCacheEntry &entry,
                                         std::uint64_t version,
                                         std::filesystem::path &snapshotPath) const;
    FileCacheResult removeEntry(const FileCacheEntry &entry) const;

    static std::string makeDocumentId(const std::string &sessionId, const std::string &remotePath);

private:
    bool ownsEntryDirectory(const std::filesystem::path &directory) const;

    std::filesystem::path m_rootDirectory;
};

#endif // DIRBRIDGE_CORE_FILECACHE_H
