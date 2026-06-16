#include "config/SettingsStore.h"

#include <fstream>
#include <stdexcept>
#include <utility>

#include <nlohmann/json.hpp>

SettingsStore::SettingsStore(std::filesystem::path path)
    : m_path(std::move(path))
{
}

const std::filesystem::path &SettingsStore::path() const
{
    return m_path;
}

UserSettings SettingsStore::load() const
{
    if (!std::filesystem::exists(m_path))
    {
        return {};
    }

    std::ifstream input(m_path);
    if (!input)
    {
        throw std::runtime_error("Failed to open user settings for reading: " + m_path.string());
    }

    const nlohmann::json document = nlohmann::json::parse(input);
    return document.get<UserSettings>();
}

void SettingsStore::save(const UserSettings &settings) const
{
    std::filesystem::create_directories(m_path.parent_path());

    std::ofstream output(m_path);
    if (!output)
    {
        throw std::runtime_error("Failed to open user settings for writing: " + m_path.string());
    }

    nlohmann::json document = settings;
    output << document.dump(4);
}
