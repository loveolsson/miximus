#pragma once
#include "nodes/connection.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace miximus::nodes {
class node;

typedef std::unordered_map<std::string, std::shared_ptr<node>> node_map_t;
typedef std::unordered_set<connection, connection_hash>        con_set_t;

class node_cfg
{
  public:
    node_map_t nodes;
    con_set_t  connections;

    node* find_node(const std::string& id) const;
    bool  erase_node(const std::string& id, std::vector<connection>& removed_connections);
    bool  remove_connection(const connection& con);
    bool  is_connection_circular(const connection& con) const;
};
} // namespace miximus::nodes