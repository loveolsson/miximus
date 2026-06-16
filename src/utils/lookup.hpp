#pragma once

#include <magic_enum/magic_enum.hpp>
#include <optional>
#include <string_view>

namespace miximus {

/**
 * Get count of values in an enum
 */
template <typename E>
constexpr auto enum_count = []() -> size_t { return magic_enum::enum_count<E>(); };

/**
 * Get enum value from an index into the enum.
 * Most likely same as enum value, but might differ.
 */
template <typename E>
constexpr auto enum_value = [](size_t i) { return magic_enum::enum_value<E>(i); };

/**
 * Get index of value in an enum.
 * Most likely same as enum value, but might differ.
 */
constexpr auto enum_index = [](auto e) -> size_t {
    const auto opt = magic_enum::enum_index(e);
    return opt.has_value() ? *opt : size_t(0); // Quiet warnings about unchecked optional
};

/**
 * Get a name of enum value.
 */
constexpr auto enum_to_string = [](auto v) -> std::string_view { return magic_enum::enum_name(v); };

/**
 * Match a string to an enum value.
 * Returns std::nullopt if no match was found.
 */
template <typename E>
constexpr std::optional<E> enum_from_string(std::string_view e)
{
    return magic_enum::enum_cast<E>(e);
}

} // namespace miximus