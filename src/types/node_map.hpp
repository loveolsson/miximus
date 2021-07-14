#pragma once
#include "types/connection_set.hpp"

#include <nlohmann/json.hpp>

#include <memory>
#include <unordered_map>

namespace miximus::nodes {

class node_i;

typedef std::unordered_map<std::string_view, con_set_t> con_map_t;

struct node_state
{
    con_map_t      con_map;
    nlohmann::json options;
};

struct node_record
{
    std::shared_ptr<node_i> node;
    node_state              state;
};

typedef std::unordered_map<std::string, node_record> node_map_t;

} // namespace miximus::nodes