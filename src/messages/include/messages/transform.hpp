#pragma once
#include "messages/types.hpp"
#include "utils/const_map.hpp"

#include <cassert>

namespace miximus::message {

constexpr auto action_lookup_table = utils::const_map_t<std::string_view, action_e>({
    {"subscribe", action_e::subscribe},
    {"unsubscribe", action_e::unsubscribe},
    {"ping", action_e::ping},
    {"socket_info", action_e::socket_info},
    {"command", action_e::command},
    {"result", action_e::result},
    {"error", action_e::error},
});

static_assert(action_lookup_table.size() == (size_t)action_e::_count);

constexpr auto action_resolve_table = utils::const_map_t<action_e, std::string_view>({
    {action_e::subscribe, "subscribe"},
    {action_e::unsubscribe, "unsubscribe"},
    {action_e::ping, "ping"},
    {action_e::socket_info, "socket_info"},
    {action_e::command, "command"},
    {action_e::result, "result"},
    {action_e::error, "error"},
});

static_assert(action_resolve_table.size() == (size_t)action_e::_count);

constexpr auto topic_lookup_table = utils::const_map_t<std::string_view, topic_e>({
    {"add_node", topic_e::add_node},
    {"remove_node", topic_e::remove_node},
    {"add_connection", topic_e::add_connection},
    {"remove_connection", topic_e::remove_connection},
    {"update_node", topic_e::update_node},
    {"config", topic_e::config},
});

static_assert(topic_lookup_table.size() == (size_t)topic_e::_count);

constexpr auto topic_resolve_table = utils::const_map_t<topic_e, std::string_view>({
    {topic_e::add_node, "add_node"},
    {topic_e::remove_node, "remove_node"},
    {topic_e::add_connection, "add_connection"},
    {topic_e::remove_connection, "remove_connection"},
    {topic_e::update_node, "update_node"},
    {topic_e::config, "config"},
});

static_assert(topic_resolve_table.size() == (size_t)topic_e::_count);

constexpr action_e action_from_string(std::string_view action)
{
    auto it = action_lookup_table.find(action);
    if (it == action_lookup_table.end()) {
        return action_e::invalid;
    }

    return it->second;
}

constexpr std::string_view get_action_string(action_e action)
{
    auto it = action_resolve_table.find(action);
    if (it == action_resolve_table.end()) {
        return "internal_error";
    }

    return it->second;
}

constexpr topic_e topic_from_string(std::string_view topic)
{
    auto it = topic_lookup_table.find(topic);
    if (it == topic_lookup_table.end()) {
        return topic_e::invalid;
    }

    return it->second;
}

constexpr std::string_view get_topic_string(topic_e topic)
{
    auto it = topic_resolve_table.find(topic);
    if (it == topic_resolve_table.end()) {
        return "internal_error";
    }

    return it->second;
}

constexpr std::string_view get_error_string(error_e error)
{
    switch (error) {
        case error_e::malformed_payload:
            return "malformed_payload";
        case error_e::no_error:
            return "no_error";
        case error_e::invalid_topic:
            return "invalid_topic";
        case error_e::invalid_type:
            return "invalid_type";
        case error_e::duplicate_id:
            return "duplicate_id";
        case error_e::invalid_options:
            return "invalid_options";

        case error_e::internal_error:
        default:
            return "internal_error";
    }
}

} // namespace miximus::message