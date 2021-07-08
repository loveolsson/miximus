#pragma once
#include "connection.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace miximus::nodes {
class node;

typedef std::unordered_map<std::string, std::shared_ptr<node>> node_map_t;
typedef std::unordered_set<connection, connection_hash>        con_set_t;

struct node_cfg
{
    node_map_t nodes;
    con_set_t  connections;

    node* find_node(const std::string& id) const;
    bool  erase_node(const std::string& id);
    bool  remove_connection(const connection& con);
    bool  is_connection_circular(const connection& con) const;
};
} // namespace miximus::nodes