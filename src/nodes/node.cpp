#include "nodes/node.hpp"

namespace miximus {

std::shared_ptr<node> node_factory::create_node(node_type_t type, std::string id)
{
    switch (type) {
        default:
            return nullptr;
    }
}

} // namespace miximus