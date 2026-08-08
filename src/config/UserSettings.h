#ifndef DIRBRIDGE_CONFIG_USERSETTINGS_H
#define DIRBRIDGE_CONFIG_USERSETTINGS_H

#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

struct RecentSession
{
    std::string siteId;
    std::string lastRemotePath = "/";
    std::string displayName;
    std::string lastOpenedAt;
};

struct UserSettings
{
    std::vector<RecentSession> recentSessions;
    bool localFileTreeVisible = true;
};

void to_json(nlohmann::json &json, const RecentSession &session);
void from_json(const nlohmann::json &json, RecentSession &session);
void to_json(nlohmann::json &json, const UserSettings &settings);
void from_json(const nlohmann::json &json, UserSettings &settings);

#endif // DIRBRIDGE_CONFIG_USERSETTINGS_H
