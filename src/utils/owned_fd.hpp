#pragma once

#include <unistd.h>
#include <utility>

namespace miximus::utils {

// Owns a POSIX descriptor. Borrowed descriptors must be duplicated before adoption.
class owned_fd_s
{
    int value_;

  public:
    explicit owned_fd_s(int value) noexcept
        : value_(value)
    {
    }
    ~owned_fd_s()
    {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    owned_fd_s(const owned_fd_s&)            = delete;
    owned_fd_s& operator=(const owned_fd_s&) = delete;
    owned_fd_s(owned_fd_s&& other) noexcept
        : value_(other.release())
    {
    }
    owned_fd_s&       operator=(owned_fd_s&&) = delete;
    int               get() const noexcept { return value_; }
    [[nodiscard]] int release() noexcept { return std::exchange(value_, -1); }
};
} // namespace miximus::utils
