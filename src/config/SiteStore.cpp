#include "config/SiteStore.h"

#include "config/PasswordCrypto.h"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

SiteStore::SiteStore(std::filesystem::path path)
    : m_path(std::move(path))
{
}

const std::filesystem::path &SiteStore::path() const
{
    return m_path;
}

std::vector<SiteProfile> SiteStore::load() const
{
    if (!std::filesystem::exists(m_path))
    {
        return {};
    }

    std::ifstream input(m_path);
    if (!input)
    {
        throw std::runtime_error("Failed to open site config for reading: " + m_path.string());
    }

    const nlohmann::json document = nlohmann::json::parse(input);
    return document.value("sites", std::vector<SiteProfile>{});
}

std::vector<std::string> SiteStore::loadGroups() const
{
    if (!std::filesystem::exists(m_path))
    {
        return {};
    }

    std::ifstream input(m_path);
    if (!input)
    {
        throw std::runtime_error("Failed to open site config for reading: " + m_path.string());
    }

    const nlohmann::json document = nlohmann::json::parse(input);
    return document.value("groups", std::vector<std::string>{});
}

void SiteStore::save(const std::vector<SiteProfile> &sites, const std::vector<std::string> &groups) const
{
    std::filesystem::create_directories(m_path.parent_path());

    nlohmann::json document;
    document["version"] = 1;
    document["passwordStorage"] = passwordStorageScheme();
    document["sites"] = sites;
    document["groups"] = groups;

    std::ofstream output(m_path);
    if (!output)
    {
        throw std::runtime_error("Failed to open site config for writing: " + m_path.string());
    }

    output << document.dump(4);
}

SiteProfile SiteStore::createDefaultSite() const
{
    SiteProfile profile;
    profile.id = "example-sftp";
    profile.name = "示例 SFTP 站点";
    profile.group = "";
    profile.protocol = RemoteProtocol::Sftp;
    profile.host = "127.0.0.1";
    profile.port = defaultPortForProtocol(profile.protocol);
    profile.username = "user";
    profile.password = "";
    profile.defaultRemotePath = "/";
    profile.encoding = "UTF-8";
    return profile;
}
