#pragma once

#include <optional>
#include <utility>

namespace miximus::utils {

/**
 * Owns the latest successfully observed value. The first observation always
 * reports a change. Use would_change()/commit() when applying a change can fail.
 */
template <typename T>
class observed_value_s
{
    std::optional<T> value_;

  public:
    observed_value_s() = default;

    explicit observed_value_s(T value)
        : value_(std::move(value))
    {
    }

    template <typename U>
    [[nodiscard]] bool would_change(const U& value) const
    {
        return !value_.has_value() || *value_ != value;
    }

    template <typename U>
    [[nodiscard]] bool observe(U&& value)
    {
        if (!would_change(value)) {
            return false;
        }

        commit(std::forward<U>(value));
        return true;
    }

    template <typename U>
    void commit(U&& value)
    {
        value_.emplace(std::forward<U>(value));
    }

    void reset() { value_.reset(); }

    [[nodiscard]] bool     has_value() const { return value_.has_value(); }
    [[nodiscard]] const T& value() const { return value_.value(); }
};

} // namespace miximus::utils
