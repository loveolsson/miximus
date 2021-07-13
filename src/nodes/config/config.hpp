#pragma once
#include "types/connection.hpp"

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
};

inline node* node_cfg::find_node(const std::string& id) const
{
    auto it = nodes.find(id);
    if (it != nodes.end()) {
        return it->second.get();
    }

    return nullptr;
}

} // namespace miximus::nodes