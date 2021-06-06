#pragma once
#include "utils/const_map.hpp"
#include <string_view>

namespace miximus::nodes {
enum class node_type_t
{
    invalid           = -1,
    decklink_producer = 0,
    decklink_consumer,
    _count,
};

constexpr auto node_type_lookup_table = utils::const_map_t<std::string_view, node_type_t>({
    {"decklink_producer", node_type_t::decklink_producer},
    {"decklink_consumer", node_type_t::decklink_consumer},
});

static_assert(node_type_lookup_table.size() == (size_t)node_type_t::_count);

constexpr node_type_t type_from_string(std::string_view type)
{
    auto it = node_type_lookup_table.find(type);
    if (it == node_type_lookup_table.end()) {
        return node_type_t::invalid;
    }

    return it->second;
}

} // namespace miximus::nodes