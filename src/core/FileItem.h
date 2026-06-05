#ifndef DIRBRIDGE_CORE_FILEITEM_H
#define DIRBRIDGE_CORE_FILEITEM_H

#include <cstdint>
#include <string>

enum class FileItemType
{
    File,
    Directory,
    Symlink,
    Other
};

struct FileItem
{
    std::string name;
    std::string path;
    FileItemType type = FileItemType::File;
    std::int64_t size = -1;
    std::string modifiedTime;
    std::string permissions;
    std::string owner;
};

std::string fileItemTypeName(FileItemType type);

#endif // DIRBRIDGE_CORE_FILEITEM_H
