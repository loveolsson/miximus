#include "nodes/node.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"

namespace miximus::nodes {

const interface_i* node_i::find_interface(std::string_view name) const
{
    auto it = interfaces_.find(name);
    if (it != interfaces_.end()) {
        return it->second;
    }
    return nullptr;
}

} // namespace miximus::nodes