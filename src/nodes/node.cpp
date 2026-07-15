#include "node.hpp"

#include "interface.hpp"
#include "validate_option.hpp"

#include <string_view>
#include <utility>

namespace miximus::nodes {
void node_i::register_interface(const interface_i* iface) { interfaces_.emplace(iface->name(), iface); }

void node_i::init(std::string_view id) { id_ = id; }

nlohmann::json node_i::get_default_options() const { return nlohmann::json::object(); }

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

error_e node_i::set_options(nlohmann::json& state, const nlohmann::json& options) const
{
    if (!state.is_object()) {
        return error_e::internal_error;
    }

    if (!options.is_object()) {
        return error_e::invalid_options;
    }

    for (auto option = options.begin(); option != options.end(); ++option) {
        const auto& key   = option.key();
        auto        value = option.value();

        if (is_valid_common_option(key, &value) || test_option(key, &value)) {
            state[key] = std::move(value);
        }
    }

    return error_e::no_error;
}

} // namespace miximus::nodes
