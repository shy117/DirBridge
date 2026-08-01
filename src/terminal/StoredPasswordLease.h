#ifndef DIRBRIDGE_TERMINAL_STOREDPASSWORDLEASE_H
#define DIRBRIDGE_TERMINAL_STOREDPASSWORDLEASE_H

#include <cstddef>
#include <string_view>
#include <utility>
#include <vector>

namespace dirbridge::terminal {

class StoredPasswordLease
{
public:
    static constexpr std::size_t MaxPasswordBytes = 1023;

    explicit StoredPasswordLease(std::string_view password);
    StoredPasswordLease(const StoredPasswordLease &) = delete;
    StoredPasswordLease &operator=(const StoredPasswordLease &) = delete;
    StoredPasswordLease(StoredPasswordLease &&other) noexcept;
    StoredPasswordLease &operator=(StoredPasswordLease &&other) noexcept;
    ~StoredPasswordLease();

    bool empty() const noexcept;
    bool consumed() const noexcept;
    std::size_t size() const noexcept;

    template<typename Consumer>
    bool consume(Consumer &&consumer)
    {
        if (consumed_)
        {
            return false;
        }

        consumed_ = true;
        try
        {
            const bool result = std::forward<Consumer>(consumer)(
                bytes_.data(),
                bytes_.size());
            clear();
            return result;
        }
        catch (...)
        {
            clear();
            throw;
        }
    }

private:
    void clear() noexcept;

    std::vector<char> bytes_;
    bool consumed_ = false;
};

} // namespace dirbridge::terminal

#endif // DIRBRIDGE_TERMINAL_STOREDPASSWORDLEASE_H
