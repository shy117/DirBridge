#include "config/SiteProfile.h"

#include "config/PasswordCrypto.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace
{
std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return value;
}
}

std::string toString(RemoteProtocol protocol)
{
    switch (protocol)
    {
    case RemoteProtocol::Ftp:
        return "ftp";
    case RemoteProtocol::Ftps:
        return "ftps";
    case RemoteProtocol::Sftp:
        return "sftp";
    case RemoteProtocol::Scp:
        return "scp";
    }

    return "sftp";
}

RemoteProtocol remoteProtocolFromString(const std::string &value)
{
    const std::string normalized = lowerCopy(value);
    if (normalized == "ftp")
    {
        return RemoteProtocol::Ftp;
    }
    if (normalized == "ftps")
    {
        return RemoteProtocol::Ftps;
    }
    if (normalized == "sftp")
    {
        return RemoteProtocol::Sftp;
    }
    if (normalized == "scp")
    {
        return RemoteProtocol::Scp;
    }

    throw std::invalid_argument("Unsupported remote protocol: " + value);
}

std::uint16_t defaultPortForProtocol(RemoteProtocol protocol)
{
    switch (protocol)
    {
    case RemoteProtocol::Ftp:
        return 21;
    case RemoteProtocol::Ftps:
        return 990;
    case RemoteProtocol::Sftp:
    case RemoteProtocol::Scp:
        return 22;
    }

    return 22;
}

void to_json(nlohmann::json &json, const SiteProfile &profile)
{
    json = nlohmann::json{
        {"id", profile.id},
        {"name", profile.name},
        {"group", profile.group},
        {"protocol", toString(profile.protocol)},
        {"host", profile.host},
        {"port", profile.port},
        {"username", profile.username},
        {"passwordStorage", passwordStorageScheme()},
        {"passwordProtected", protectPassword(profile.password)},
        {"defaultRemotePath", profile.defaultRemotePath},
        {"encoding", profile.encoding}
    };
}

void from_json(const nlohmann::json &json, SiteProfile &profile)
{
    profile.id = json.value("id", "");
    profile.name = json.value("name", "");
    profile.group = json.value("group", "");
    profile.protocol = remoteProtocolFromString(json.value("protocol", "sftp"));
    profile.host = json.value("host", "");
    profile.port = json.value("port", defaultPortForProtocol(profile.protocol));
    profile.username = json.value("username", "");
    if (json.contains("passwordProtected"))
    {
        profile.password = unprotectPassword(json.value("passwordProtected", ""));
    }
    else
    {
        profile.password = json.value("password", "");
    }
    profile.defaultRemotePath = json.value("defaultRemotePath", "/");
    profile.encoding = json.value("encoding", "UTF-8");
}
