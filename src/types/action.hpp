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

constexpr auto action_from_string = enum_from_string<action_e>;

} // namespace miximus