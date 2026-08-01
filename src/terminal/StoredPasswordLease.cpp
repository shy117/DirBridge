#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "terminal/StoredPasswordLease.h"

#include <stdexcept>

namespace dirbridge::terminal {

StoredPasswordLease::StoredPasswordLease(std::string_view password)
{
    if (password.size() > MaxPasswordBytes)
    {
        throw std::invalid_argument("Stored SSH password is too long");
    }
    for (const char character : password)
    {
        if (character == '\0' || character == '\r' || character == '\n')
        {
            throw std::invalid_argument(
                "Stored SSH password contains an unsupported line break");
        }
    }
    bytes_.assign(password.begin(), password.end());
}

StoredPasswordLease::StoredPasswordLease(StoredPasswordLease &&other) noexcept
    : consumed_(other.consumed_)
{
    bytes_.swap(other.bytes_);
    other.consumed_ = true;
}

StoredPasswordLease &StoredPasswordLease::operator=(
    StoredPasswordLease &&other) noexcept
{
    if (this != &other)
    {
        clear();
        bytes_.swap(other.bytes_);
        consumed_ = other.consumed_;
        other.consumed_ = true;
    }
    return *this;
}

StoredPasswordLease::~StoredPasswordLease()
{
    clear();
}

bool StoredPasswordLease::empty() const noexcept
{
    return bytes_.empty();
}

bool StoredPasswordLease::consumed() const noexcept
{
    return consumed_;
}

std::size_t StoredPasswordLease::size() const noexcept
{
    return bytes_.size();
}

void StoredPasswordLease::clear() noexcept
{
    if (!bytes_.empty())
    {
        SecureZeroMemory(bytes_.data(), bytes_.size());
        bytes_.clear();
    }
}

} // namespace dirbridge::terminal
