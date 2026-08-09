#ifndef DIRBRIDGE_CONFIG_SITESTORE_H
#define DIRBRIDGE_CONFIG_SITESTORE_H

#include <filesystem>
#include <string>
#include <vector>

#include "config/SiteProfile.h"

class SiteStore
{
public:
    explicit SiteStore(std::filesystem::path path);

    const std::filesystem::path &path() const;
    std::vector<SiteProfile> load() const;
    std::vector<std::string> loadGroups() const;
    void save(const std::vector<SiteProfile> &sites, const std::vector<std::string> &groups = {}) const;
    SiteProfile createDefaultSite() const;

private:
    std::filesystem::path m_path;
};

#endif // DIRBRIDGE_CONFIG_SITESTORE_H
