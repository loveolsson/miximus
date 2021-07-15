#pragma once
#include "types/connection_set.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <unordered_map>

namespace miximus::nodes {

class node_i;

using con_map_t = std::unordered_map<std::string_view, con_set_t>;

struct node_state
{
    con_map_t      con_map;
    nlohmann::json options;
    bool           executed{false};

    const con_set_t& get_connections(std::string_view name) const
    {
        auto it = con_map.find(name);
        if (it == con_map.end()) {
            throw std::runtime_error(std::string("missing connection set ") + std::string(name));
        }
        return it->second;
    }

    template <typename T>
    T get_option(std::string_view name, T fallback) const
    {
        auto it = options.find(name);
        if (it == options.end()) {
            return fallback;
        }

        try {
            return it->get<T>();
        } catch (nlohmann::json::exception& e) {
            return fallback;
        }
    }
};

struct node_record
{
    std::shared_ptr<node_i> node;
    node_state              state;
};

using node_map_t = std::unordered_map<std::string, node_record>;

} // namespace miximus::nodes