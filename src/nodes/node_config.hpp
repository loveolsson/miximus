#pragma once
#include "connection.hpp"

#include <memory>
#include <string>
#include <unordered_map>

namespace miximus::nodes {
class node;

struct node_cfg_t
{
    typedef std::unordered_map<std::string, std::shared_ptr<node>> node_map_t;
    typedef std::unordered_map<size_t, connection>                 con_map_t;

    node_map_t nodes;
    con_map_t  connections;

    node* find_node(const std::string& id) const
    {
        auto it = nodes.find(id);
        if (it != nodes.end()) {
            return it->second.get();
        }

        return nullptr;
    }

    bool erase_node(const std::string& id)
    {
        auto it = nodes.find(id);
        if (it != nodes.end()) {
            nodes.erase(it);
            return true;
        }

        return false;
    }

    const connection* find_connection(size_t hash) const
    {
        auto it = connections.find(hash);
        if (it != connections.end()) {
            return &it->second;
        }

        return nullptr;
    }

    bool erase_node(size_t hash)
    {
        auto it = connections.find(hash);
        if (it != connections.end()) {
            connections.erase(it);
            return true;
        }

        return false;
    }

    bool erase_node(const connection& con) { return erase_node(std::hash<connection>{}(con)); }
};
} // namespace miximus::nodes