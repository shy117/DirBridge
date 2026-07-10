#ifndef DIRBRIDGE_CONFIG_SETTINGSSTORE_H
#define DIRBRIDGE_CONFIG_SETTINGSSTORE_H

#include "config/UserSettings.h"

#include <filesystem>

class SettingsStore
{
public:
    /**
     * @brief 创建一个由指定 JSON 文件支撑的设置存储。
     * @param path `settings.json` 的文件路径。
     */
    explicit SettingsStore(std::filesystem::path path);

    /**
     * @brief 返回当前存储使用的文件路径。
     * @return 稳定的设置 JSON 路径。
     */
    const std::filesystem::path &path() const;

    /**
     * @brief 加载用户设置；文件不存在时返回默认值。
     * @return 解析后的设置对象。
     */
    UserSettings load() const;

    /**
     * @brief 将用户设置保存到磁盘，并在需要时创建父目录。
     * @param settings 要写入的设置对象。
     */
    void save(const UserSettings &settings) const;

private:
    std::filesystem::path m_path;
};

#endif // DIRBRIDGE_CONFIG_SETTINGSSTORE_H
