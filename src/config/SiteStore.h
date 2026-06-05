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
    void save(const std::vector<SiteProfile> &sites) const;
    SiteProfile createDefaultSite() const;

private:
    std::filesystem::path m_path;
};

#endif // DIRBRIDGE_CONFIG_SITESTORE_H
