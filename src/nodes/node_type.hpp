#pragma once
#include <frozen/map.h>

#include <string_view>

namespace miximus::nodes {
enum class node_type_e
{
    invalid      = -1,
    math_add_i64 = 0,
    math_add_f64,
    math_add_vec2,
    math_sub_i64,
    math_sub_f64,
    math_sub_vec2,
    math_mul_i64,
    math_mul_f64,
    math_mul_vec2,
    math_min_i64,
    math_min_f64,
    math_min_vec2,
    math_max_i64,
    math_max_f64,
    math_max_vec2,
    decklink_producer,
    decklink_consumer,
    _count,
};

constexpr frozen::map<std::string_view, node_type_e, (size_t)node_type_e::_count> node_type_lookup_table = {
    {"math_add_i64", node_type_e::math_add_i64},
    {"math_add_f64", node_type_e::math_add_f64},
    {"math_add_vec2", node_type_e::math_add_vec2},
    {"math_sub_i64", node_type_e::math_sub_i64},
    {"math_sub_f64", node_type_e::math_sub_f64},
    {"math_sub_vec2", node_type_e::math_sub_vec2},
    {"math_mul_i64", node_type_e::math_mul_i64},
    {"math_mul_f64", node_type_e::math_mul_f64},
    {"math_mul_vec2", node_type_e::math_mul_vec2},
    {"math_min_i64", node_type_e::math_min_i64},
    {"math_min_f64", node_type_e::math_min_f64},
    {"math_min_vec2", node_type_e::math_min_vec2},
    {"math_max_i64", node_type_e::math_max_i64},
    {"math_max_f64", node_type_e::math_max_f64},
    {"math_max_vec2", node_type_e::math_max_vec2},
    {"decklink_producer", node_type_e::decklink_producer},
    {"decklink_consumer", node_type_e::decklink_consumer},
};

constexpr frozen::map<node_type_e, std::string_view, (size_t)node_type_e::_count> node_string_lookup_table = {
    {node_type_e::math_add_i64, "math_add_i64"},
    {node_type_e::math_add_f64, "math_add_f64"},
    {node_type_e::math_add_vec2, "math_add_vec2"},
    {node_type_e::math_sub_i64, "math_sub_i64"},
    {node_type_e::math_sub_f64, "math_sub_f64"},
    {node_type_e::math_sub_vec2, "math_sub_vec2"},
    {node_type_e::math_mul_i64, "math_mul_i64"},
    {node_type_e::math_mul_f64, "math_mul_f64"},
    {node_type_e::math_mul_vec2, "math_mul_vec2"},
    {node_type_e::math_min_i64, "math_min_i64"},
    {node_type_e::math_min_f64, "math_min_f64"},
    {node_type_e::math_min_vec2, "math_min_vec2"},
    {node_type_e::math_max_i64, "math_max_i64"},
    {node_type_e::math_max_f64, "math_max_f64"},
    {node_type_e::math_max_vec2, "math_max_vec2"},
    {node_type_e::decklink_producer, "decklink_producer"},
    {node_type_e::decklink_consumer, "decklink_consumer"},
};

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