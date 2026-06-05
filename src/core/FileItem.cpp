#include "core/FileItem.h"

std::string fileItemTypeName(FileItemType type)
{
    switch (type)
    {
    case FileItemType::File:
        return "file";
    case FileItemType::Directory:
        return "directory";
    case FileItemType::Symlink:
        return "symlink";
    case FileItemType::Other:
        return "other";
    }

    return "other";
}
