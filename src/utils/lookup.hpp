#pragma once

#include <magic_enum/magic_enum.hpp>
#include <magic_enum/magic_enum_containers.hpp>

#include <optional>
#include <string_view>

namespace miximus {

template <typename E, typename T>
using enum_array_t = magic_enum::containers::array<E, T>;

/**
 * Get a name of enum value.
 */
constexpr auto enum_to_string = [](auto v) -> std::string_view { return magic_enum::enum_name(v); };

/**
 * Match a string to an enum value.
 * Returns std::nullopt if no match was found.
 */
template <typename E>
[[nodiscard]] constexpr std::optional<E> enum_from_string(std::string_view e)
{
    return magic_enum::enum_cast<E>(e);
}

} // namespace miximus
