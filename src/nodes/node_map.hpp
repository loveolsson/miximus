#pragma once
#include "nodes/connection.hpp"
#include "utils/lookup.hpp"

#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <vector>

namespace miximus::nodes {

class node_i;
class interface_i;

// Set of connections connected to an interface
using con_set_t = std::vector<connection_s>;

// Map of connection sets, keyed of interface name
using con_map_t = std::map<std::string_view, con_set_t>;

// Map of interfaces stored on each node
using interface_map_t = std::map<std::string_view, const interface_i*>;

/**
 * Configuration state of a node
 * This is stored separate from the node and can be copied without
 * involving the node instance
 */
struct node_state_s
{
    con_map_t      con_map;
    nlohmann::json options;
    mutable bool   executed{false};

    const con_set_t& get_connection_set(std::string_view name) const
    {
        if (auto it = con_map.find(name); it != con_map.end()) {
            return it->second;
        }

        throw std::runtime_error(fmt::format("missing connection set {}", name));
    }

    template <typename T>
    T get_option(std::string_view name, const T& fallback = T()) const
    {
        const auto it = options.find(name);
        if (it == options.end()) {
            return fallback;
        }

        try {
            return it->get<T>();
        } catch (nlohmann::json::exception& e) {
            return fallback;
        }
    }

    template <typename T>
    T get_enum_option(std::string_view name, T fallback) const
    {
        const auto opt = get_option<std::string_view>(name);
        const auto e   = enum_from_string<T>(opt);

        if (e.has_value()) {
            return *e;
        } else {
            return fallback;
        }
    }
};

/**
 * Record of a node contaning an owning reference to the node
 * and a copy of it's config
 */
struct node_record_s
{
    std::shared_ptr<node_i> node;
    node_state_s            state;
};

// Map of node records, keyed of node id
using node_map_t = std::unordered_map<std::string, node_record_s>;

} // namespace miximus::nodes