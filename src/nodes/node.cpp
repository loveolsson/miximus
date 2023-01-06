#include "node.hpp"
#include "validate_option.hpp"

namespace miximus::nodes {
bool node_i::is_valid_common_option(std::string_view name, nlohmann::json* value)
{
    if (name == "node_visual_position") {
        return nodes::validate_option<gpu::vec2_t>(value);
    }

    if (name == "name") {
        return nodes::validate_option<std::string_view>(value);
    }

    return false;
}

} // namespace miximus::nodes