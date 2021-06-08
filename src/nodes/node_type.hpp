#pragma once
#include "utils/const_map.hpp"
#include <string_view>

namespace miximus::nodes {
enum class node_type_e
{
    invalid           = -1,
    decklink_producer = 0,
    decklink_consumer,
    _count,
};

constexpr auto node_type_lookup_table = utils::const_map_t<std::string_view, node_type_e>({
    {"decklink_producer", node_type_e::decklink_producer},
    {"decklink_consumer", node_type_e::decklink_consumer},
});

constexpr auto node_string_lookup_table = utils::const_map_t<node_type_e, std::string_view>({
    {node_type_e::decklink_producer, "decklink_producer"},
    {node_type_e::decklink_consumer, "decklink_consumer"},
});

static_assert(node_type_lookup_table.size() == (size_t)node_type_e::_count);

constexpr node_type_e type_from_string(std::string_view type)
{
    auto it = node_type_lookup_table.find(type);
    if (it == node_type_lookup_table.end()) {
        return node_type_e::invalid;
    }

    return it->second;
}

constexpr std::string_view type_from_string(node_type_e type)
{
    auto it = node_string_lookup_table.find(type);
    if (it == node_string_lookup_table.end()) {
        return "invalid";
    }

    return it->second;
}

} // namespace miximus::nodes