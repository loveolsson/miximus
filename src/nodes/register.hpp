#pragma once
#include <functional>
#include <map>
#include <memory>
#include <string_view>

namespace miximus::nodes {

class node_i;

using constructor_t     = std::function<std::shared_ptr<node_i>()>;
using constructor_map_t = std::map<std::string_view, constructor_t>;

void register_nodes(constructor_map_t* map);

} // namespace miximus::nodes
