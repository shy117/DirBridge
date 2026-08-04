#ifndef DIRBRIDGE_CONFIG_SITEPROFILE_H
#define DIRBRIDGE_CONFIG_SITEPROFILE_H

#include <cstdint>
#include <string>

#include <nlohmann/json_fwd.hpp>

enum class RemoteProtocol
{
    Ftp,
    Ftps,
    Sftp,
    Scp
};

struct SiteProfile
{
    std::string id;
    std::string name;
    std::string group;
    RemoteProtocol protocol = RemoteProtocol::Sftp;
    std::string host;
    std::uint16_t port = 22;
    std::string username;
    std::string password;
    std::string defaultRemotePath = "/";
    std::string encoding = "UTF-8";
    bool fileTreeVisible = true;
};

std::string toString(RemoteProtocol protocol);
RemoteProtocol remoteProtocolFromString(const std::string &value);
std::uint16_t defaultPortForProtocol(RemoteProtocol protocol);

void to_json(nlohmann::json &json, const SiteProfile &profile);
void from_json(const nlohmann::json &json, SiteProfile &profile);

#endif // DIRBRIDGE_CONFIG_SITEPROFILE_H
