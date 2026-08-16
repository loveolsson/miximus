#pragma once
#include "nodes/node_fwd.hpp"
#include "nodes/node_map_fwd.hpp"
#include "utils/lookup.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <format>
#include <memory>
#include <stdexcept>
#include <type_traits>

namespace miximus::nodes {

/**
 * Configuration state of a node
 * This is stored separate from the node and can be copied without
 * involving the node instance
 */
struct node_state_s
{
    con_map_t      con_map;
    nlohmann::json options;

    const con_set_t& get_connection_set(std::string_view name) const
    {
        if (auto it = con_map.find(name); it != con_map.end()) {
            return it->second;
        }

        throw std::runtime_error(std::format("missing connection set {}", name));
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
        requires std::is_enum_v<T>
    T get_enum_option_unchecked(std::string_view name) const
    {
        const auto option = options.find(name);
        assert(option != options.end());

        const auto* value = option->get_ptr<const nlohmann::json::string_t*>();
        assert(value != nullptr);

        const auto result = enum_from_string<T>(*value);
        assert(result.has_value());
        return *result;
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

} // namespace miximus::nodes
