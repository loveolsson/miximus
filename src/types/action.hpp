#pragma once
#include "utils/lookup.hpp"

#include <frozen/map.h>

#include <string_view>

namespace miximus {

enum class action_e
{
    invalid   = -1,
    subscribe = 0,
    unsubscribe,
    ping,
    socket_info,
    command,
    result,
    error,
    _count,
};

constexpr frozen::map<std::string_view, action_e, (size_t)action_e::_count> action_lookup_table = {
    {"subscribe", action_e::subscribe},
    {"unsubscribe", action_e::unsubscribe},
    {"ping", action_e::ping},
    {"socket_info", action_e::socket_info},
    {"command", action_e::command},
    {"result", action_e::result},
    {"error", action_e::error},
};

constexpr frozen::map<action_e, std::string_view, (size_t)action_e::_count> action_resolve_table = {
    {action_e::subscribe, "subscribe"},
    {action_e::unsubscribe, "unsubscribe"},
    {action_e::ping, "ping"},
    {action_e::socket_info, "socket_info"},
    {action_e::command, "command"},
    {action_e::result, "result"},
    {action_e::error, "error"},
};

static_assert(verify_lookup(action_lookup_table, action_resolve_table), "Lookup tables does not match");

constexpr action_e action_from_string(std::string_view action)
{
    auto it = action_lookup_table.find(action);
    if (it == action_lookup_table.end()) {
        return action_e::invalid;
    }

    return it->second;
}

constexpr std::string_view action_to_string(action_e action)
{
    auto it = action_resolve_table.find(action);
    if (it == action_resolve_table.end()) {
        return "internal_error";
    }

    return it->second;
}

} // namespace miximus