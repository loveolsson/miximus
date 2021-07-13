#pragma once
#include <frozen/map.h>
#include <string_view>

namespace miximus {

enum class topic_e
{
    invalid  = -1,
    add_node = 0,
    remove_node,
    add_connection,
    remove_connection,
    update_node,
    config,
    _count,
};

constexpr frozen::map<std::string_view, topic_e, (size_t)topic_e::_count> topic_lookup_table = {
    {"add_node", topic_e::add_node},
    {"remove_node", topic_e::remove_node},
    {"add_connection", topic_e::add_connection},
    {"remove_connection", topic_e::remove_connection},
    {"update_node", topic_e::update_node},
    {"config", topic_e::config},
};

constexpr frozen::map<topic_e, std::string_view, (size_t)topic_e::_count> topic_resolve_table = {
    {topic_e::add_node, "add_node"},
    {topic_e::remove_node, "remove_node"},
    {topic_e::add_connection, "add_connection"},
    {topic_e::remove_connection, "remove_connection"},
    {topic_e::update_node, "update_node"},
    {topic_e::config, "config"},
};

constexpr topic_e topic_from_string(std::string_view topic)
{
    auto it = topic_lookup_table.find(topic);
    if (it == topic_lookup_table.end()) {
        return topic_e::invalid;
    }

    return it->second;
}

constexpr std::string_view topic_to_string(topic_e topic)
{
    auto it = topic_resolve_table.find(topic);
    if (it == topic_resolve_table.end()) {
        return "internal_error";
    }

    return it->second;
}

} // namespace miximus