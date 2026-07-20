#include "register.hpp"

#include <memory>

namespace miximus::nodes::generators {

std::shared_ptr<node_i> create_test_pattern_node();

void register_nodes(node_definition_map_t* map) { map->emplace("test_pattern", create_test_pattern_node); }

} // namespace miximus::nodes::generators
