#include "node.hpp"

#include "interface.hpp"
#include "normalize_option.hpp"

#include <format>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace miximus::nodes {
void node_i::register_interface(const interface_i& iface)
{
    const auto [_, inserted] = interfaces_.emplace(iface.name(), &iface);
    if (!inserted) {
        throw std::logic_error(std::format("duplicate interface name {}", iface.name()));
    }
}

void node_i::init(std::string_view id) { id_ = id; }

nlohmann::json node_i::get_default_options() const { return nlohmann::json::object(); }

option_result_e node_i::normalize_common_option(std::string_view name, nlohmann::json* value)
{
    if (name == "node_visual_position") {
        return nodes::normalize_option_value<gpu::vec2_t>(value);
    }

    if (name == "name") {
        return nodes::normalize_option_value<std::string_view>(value);
    }

    return option_result_e::invalid;
}

set_options_result_s node_i::set_options(nlohmann::json& state, const nlohmann::json& options) const
{
    if (!state.is_object()) {
        return {error_e::internal_error, false};
    }

    if (!options.is_object()) {
        return {error_e::invalid_options, false};
    }

    auto normalized_state     = state;
    bool has_corrected_values = false;

    for (auto option = options.begin(); option != options.end(); ++option) {
        const auto& key   = option.key();
        auto        value = option.value();

        auto result = normalize_common_option(key, &value);
        if (result == option_result_e::invalid) {
            result = normalize_option(key, &value);
        }

        if (result == option_result_e::invalid) {
            return {error_e::invalid_options, false};
        }

        has_corrected_values |= result == option_result_e::corrected;
        normalized_state[key] = std::move(value);
    }

    state = std::move(normalized_state);
    return {error_e::no_error, has_corrected_values};
}

} // namespace miximus::nodes
