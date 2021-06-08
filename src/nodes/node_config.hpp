#pragma once
#include "connection.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace miximus::nodes {
class node;

struct node_cfg_t
{
    typedef std::unordered_map<std::string, std::shared_ptr<node>> node_map_t;
    typedef std::unordered_set<connection>                         con_map_t;

    node_map_t nodes;
    con_map_t  connections;

    node* find_node(const std::string& id) const;
    bool  erase_node(const std::string& id);
    bool  erase_connection(const connection& con);
};
} // namespace miximus::nodes