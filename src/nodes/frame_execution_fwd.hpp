#pragma once

#include <string_view>
#include <unordered_set>

namespace miximus::nodes {

using submitted_node_set_t = std::unordered_set<std::string_view>;
using executed_node_set_t  = std::unordered_set<std::string_view>;

} // namespace miximus::nodes
