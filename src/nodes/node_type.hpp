#pragma once
#include "utils/lookup.hpp"

#include <frozen/map.h>

#include <string_view>

namespace miximus::nodes {
enum class node_type_e
{
    invalid  = -1,
    math_i64 = 0,
    math_f64,
    math_vec2,
    decklink_producer,
    decklink_consumer,
    _count,
};

constexpr frozen::map<std::string_view, node_type_e, (size_t)node_type_e::_count> node_type_lookup_table = {
    {"math_i64", node_type_e::math_i64},
    {"math_f64", node_type_e::math_f64},
    {"math_vec2", node_type_e::math_vec2},
    {"decklink_producer", node_type_e::decklink_producer},
    {"decklink_consumer", node_type_e::decklink_consumer},
};

constexpr frozen::map<node_type_e, std::string_view, (size_t)node_type_e::_count> node_string_lookup_table = {
    {node_type_e::math_i64, "math_i64"},
    {node_type_e::math_f64, "math_f64"},
    {node_type_e::math_vec2, "math_vec2"},
    {node_type_e::decklink_producer, "decklink_producer"},
    {node_type_e::decklink_consumer, "decklink_consumer"},
};

static_assert(verify_lookup(node_type_lookup_table, node_string_lookup_table), "Lookup tables does not match");

constexpr node_type_e type_from_string(std::string_view type)
{
    auto it = node_type_lookup_table.find(type);
    if (it == node_type_lookup_table.end()) {
        return node_type_e::invalid;
    }

    return it->second;
}

constexpr std::string_view type_to_string(node_type_e type)
{
    auto it = node_string_lookup_table.find(type);
    if (it == node_string_lookup_table.end()) {
        return "invalid";
    }

    return it->second;
}

} // namespace miximus::nodes