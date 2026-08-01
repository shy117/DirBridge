#ifndef DIRBRIDGE_TERMINAL_STOREDPASSWORDCHANNEL_H
#define DIRBRIDGE_TERMINAL_STOREDPASSWORDCHANNEL_H

#include "terminal/StoredPasswordLease.h"

#include <filesystem>
#include <memory>
#include <string>

namespace dirbridge::terminal {

struct StoredPasswordEndpoint
{
    std::wstring pipeName;
    std::wstring token;
};

class StoredPasswordChannel
{
public:
    static std::unique_ptr<StoredPasswordChannel> create(
        StoredPasswordLease password,
        std::filesystem::path expectedHelperPath,
        std::string &error);

    StoredPasswordChannel(const StoredPasswordChannel &) = delete;
    StoredPasswordChannel &operator=(const StoredPasswordChannel &) = delete;
    StoredPasswordChannel(StoredPasswordChannel &&) = delete;
    StoredPasswordChannel &operator=(StoredPasswordChannel &&) = delete;
    ~StoredPasswordChannel();

    const StoredPasswordEndpoint &endpoint() const noexcept;
    bool serveOnce(std::string &error);
    void cancel() noexcept;

private:
    struct Impl;
    explicit StoredPasswordChannel(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace dirbridge::terminal

#endif // DIRBRIDGE_TERMINAL_STOREDPASSWORDCHANNEL_H
