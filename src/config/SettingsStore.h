#ifndef DIRBRIDGE_CONFIG_SETTINGSSTORE_H
#define DIRBRIDGE_CONFIG_SETTINGSSTORE_H

#include "config/UserSettings.h"

#include <filesystem>

class SettingsStore
{
public:
    /**
     * @brief Creates a settings store backed by the given JSON file.
     * @param path Path to `settings.json`.
     */
    explicit SettingsStore(std::filesystem::path path);

    /**
     * @brief Returns the file path used by this store.
     * @return Stable settings JSON path.
     */
    const std::filesystem::path &path() const;

    /**
     * @brief Loads user settings, returning defaults when the file does not exist.
     * @return Parsed settings document.
     */
    UserSettings load() const;

    /**
     * @brief Saves user settings to disk, creating the parent directory when needed.
     * @param settings Settings document to write.
     */
    void save(const UserSettings &settings) const;

private:
    std::filesystem::path m_path;
};

#endif // DIRBRIDGE_CONFIG_SETTINGSSTORE_H
