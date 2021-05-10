#pragma once
#include "messages/types.hpp"
#include "utils/const_map.hpp"

#include <cassert>

namespace miximus::message {

constexpr auto action_lookup_table = utils::const_map_t<std::string_view, action_t>({
    {"subscribe", action_t::subscribe},
    {"unsubscribe", action_t::unsubscribe},
    {"ping", action_t::ping},
    {"socket_info", action_t::socket_info},
    {"command", action_t::command},
    {"result", action_t::result},
    {"error", action_t::error},
});

static_assert(action_lookup_table.size() == (size_t)action_t::_count);

constexpr auto action_resolve_table = utils::const_map_t<action_t, std::string_view>({
    {action_t::subscribe, "subscribe"},
    {action_t::unsubscribe, "unsubscribe"},
    {action_t::ping, "ping"},
    {action_t::socket_info, "socket_info"},
    {action_t::command, "command"},
    {action_t::result, "result"},
    {action_t::error, "error"},
});

static_assert(action_resolve_table.size() == (size_t)action_t::_count);

constexpr auto topic_lookup_table = utils::const_map_t<std::string_view, topic_t>({
    {"add_node", topic_t::add_node},
    {"remove_node", topic_t::remove_node},
    {"add_connection", topic_t::add_connection},
    {"remove_connection", topic_t::remove_connection},
    {"update_node", topic_t::update_node},
    {"config", topic_t::config},
});

static_assert(topic_lookup_table.size() == (size_t)topic_t::_count);

constexpr auto topic_resolve_table = utils::const_map_t<topic_t, std::string_view>({
    {topic_t::add_node, "add_node"},
    {topic_t::remove_node, "remove_node"},
    {topic_t::add_connection, "add_connection"},
    {topic_t::remove_connection, "remove_connection"},
    {topic_t::update_node, "update_node"},
    {topic_t::config, "config"},
});

static_assert(topic_resolve_table.size() == (size_t)topic_t::_count);

constexpr action_t action_from_string(std::string_view action)
{
    auto it = action_lookup_table.find(action);
    if (it == action_lookup_table.end()) {
        return action_t::invalid;
    }

    return it->second;
}

constexpr std::string_view get_action_string(action_t action)
{
    auto it = action_resolve_table.find(action);
    if (it == action_resolve_table.end()) {
        return "internal_error";
    }

    return it->second;
}

constexpr topic_t topic_from_string(std::string_view topic)
{
    auto it = topic_lookup_table.find(topic);
    if (it == topic_lookup_table.end()) {
        return topic_t::invalid;
    }

    return it->second;
}

constexpr std::string_view get_topic_string(topic_t topic)
{
    auto it = topic_resolve_table.find(topic);
    if (it == topic_resolve_table.end()) {
        return "internal_error";
    }

    return it->second;
}

constexpr std::string_view get_error_string(error_t error)
{
    switch (error) {
        case error_t::malformed_payload:
            return "malformed_payload";
        case error_t::no_error:
            return "no_error";
        case error_t::invalid_topic:
            return "invalid_topic";
        case error_t::invalid_type:
            return "invalid_type";
        case error_t::duplicate_id:
            return "duplicate_id";

        case error_t::internal_error:
        default:
            return "internal_error";
    }
}

} // namespace miximus::message