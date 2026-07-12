#pragma once
#include "utils/lookup.hpp"

namespace miximus {

enum class action_e
{
    subscribe,
    unsubscribe,
    ping,
    socket_info,
    command,
    result,
    error,
};

constexpr std::optional<action_e> action_from_string(std::string_view value)
{
    return enum_from_string<action_e>(value);
}

} // namespace miximus
