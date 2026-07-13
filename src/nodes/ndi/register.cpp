#include "register.hpp"

#include <memory>

namespace miximus::nodes::ndi {

std::shared_ptr<node_i> create_input_node();
std::shared_ptr<node_i> create_output_node();

void register_nodes(node_definition_map_t* map)
{
    map->emplace("ndi_input", ndi::create_input_node);
    map->emplace("ndi_output", ndi::create_output_node);
}

} // namespace miximus::nodes::ndi
