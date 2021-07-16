#include "nodes/node.hpp"
#include "logger/logger.hpp"
#include "nodes/dummy/dummy.hpp"
#include "nodes/interface.hpp"
#include "nodes/math/math.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes {

interface_i* node_i::find_interface(std::string_view name)
{
    auto it = interfaces_.find(name);
    if (it != interfaces_.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<node_i> create_node(node_type_e type, error_e& error)
{
    switch (type) {
        case node_type_e::math_i64:
        case node_type_e::math_f64:
        case node_type_e::math_vec2:
            return math::create_node(type);

        case node_type_e::decklink_producer:
            error = error_e::invalid_type;
            return nullptr;
        case node_type_e::decklink_consumer:
            error = error_e::invalid_type;
            return nullptr;

        default:
#if 1
            error = error_e::invalid_type;
            return nullptr;
#else
            return dummy::create_node(type);
#endif
    }
}

} // namespace miximus::nodes