#pragma once
#include "types/connection_set.hpp"
#include <memory>
#include <unordered_map>

namespace miximus::nodes {

class node_i;

typedef std::unordered_map<std::string_view, con_set_t> con_map_t;

struct node_state
{
    std::shared_ptr<node_i> node;
    con_map_t               con_map;
};

typedef std::unordered_map<std::string, node_state> node_map_t;

} // namespace miximus::nodes