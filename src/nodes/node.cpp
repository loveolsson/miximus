#include "nodes/node.hpp"
#include "logger/logger.hpp"
#include "nodes/dummy/dummy.hpp"
#include "nodes/interface.hpp"
#include "nodes/math/math.hpp"

#include <frozen/map.h>
#include <nlohmann/json.hpp>

namespace miximus::nodes {

const interface_i* node_i::find_interface(std::string_view name) const
{
    auto it = interfaces_.find(name);
    if (it != interfaces_.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<node_i> create_node(node_i::type_e type, error_e& error)
{
    using type_e = node_i::type_e;

    switch (type) {
        case type_e::math_i64:
        case type_e::math_f64:
        case type_e::math_vec2:
            return math::create_node(type);

        case type_e::decklink_producer:
            error = error_e::invalid_type;
            return nullptr;
        case type_e::decklink_consumer:
            error = error_e::invalid_type;
            return nullptr;

        default:
#if 1
            error = error_e::invalid_type;
            return nullptr;
#else
            return dummy::create_node(type);
#endif
    }
}

constexpr frozen::map<std::string_view, node_i::type_e, (size_t)node_i::type_e::_count> node_type_lookup_table = {
    {"math_i64", node_i::type_e::math_i64},
    {"math_f64", node_i::type_e::math_f64},
    {"math_vec2", node_i::type_e::math_vec2},
    {"decklink_producer", node_i::type_e::decklink_producer},
    {"decklink_consumer", node_i::type_e::decklink_consumer},
};

constexpr frozen::map<node_i::type_e, std::string_view, (size_t)node_i::type_e::_count> node_string_lookup_table = {
    {node_i::type_e::math_i64, "math_i64"},
    {node_i::type_e::math_f64, "math_f64"},
    {node_i::type_e::math_vec2, "math_vec2"},
    {node_i::type_e::decklink_producer, "decklink_producer"},
    {node_i::type_e::decklink_consumer, "decklink_consumer"},
};

static_assert(verify_lookup(node_type_lookup_table, node_string_lookup_table), "Lookup tables does not match");

node_i::type_e node_i::type_from_string(std::string_view type)
{
    auto it = node_type_lookup_table.find(type);
    if (it == node_type_lookup_table.end()) {
        return type_e::invalid;
    }

    return it->second;
}

std::string_view node_i::type_to_string(node_i::type_e type)
{
    auto it = node_string_lookup_table.find(type);
    if (it == node_string_lookup_table.end()) {
        return "invalid";
    }

    return it->second;
}

} // namespace miximus::nodes