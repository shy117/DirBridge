#include "config/UserSettings.h"

#include <nlohmann/json.hpp>

void to_json(nlohmann::json &json, const RecentSession &session)
{
    json = nlohmann::json{
        {"siteId", session.siteId},
        {"lastRemotePath", session.lastRemotePath},
        {"displayName", session.displayName},
        {"lastOpenedAt", session.lastOpenedAt}
    };
}

void from_json(const nlohmann::json &json, RecentSession &session)
{
    session.siteId = json.value("siteId", "");
    session.lastRemotePath = json.value("lastRemotePath", "/");
    session.displayName = json.value("displayName", "");
    session.lastOpenedAt = json.value("lastOpenedAt", "");
}

void to_json(nlohmann::json &json, const UserSettings &settings)
{
    json = nlohmann::json{
        {"version", 1},
        {"recentSessions", settings.recentSessions}
    };
}

void from_json(const nlohmann::json &json, UserSettings &settings)
{
    settings.recentSessions = json.value("recentSessions", std::vector<RecentSession>{});
}
