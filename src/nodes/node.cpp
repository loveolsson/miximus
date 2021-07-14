#include "nodes/node.hpp"
#include "logger/logger.hpp"
#include "nodes/dummy/dummy.hpp"
#include "nodes/interface.hpp"
#include "nodes/math/math.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes {

node_i::node_i()
{
    options_.emplace("position", &opt_position_);
    options_.emplace("name", &opt_name_);
}

void node_i::complete()
{
    for (auto& [name, iface] : interfaces_) {
        (void)name;
        iface->reset();
    }
}

bool node_i::set_option(std::string_view option, const nlohmann::json& value)
{
    auto it = options_.find(option);

    if (it != options_.end()) {
        return it->second->set_json(value);
    }

    return false;
}

nlohmann::json node_i::get_options()
{
    auto options = nlohmann::json::object();

    for (auto& [name, option] : options_) {
        options.emplace(name, option->get_json());
    }

    return options;
}

nlohmann::json node_i::get_option(std::string_view option)
{
    auto it = options_.find(option);
    if (it != options_.end()) {
        return it->second->get_json();
    }

    return nlohmann::json(); // null
}

interface_i* node_i::find_interface(std::string_view name)
{
    auto it = interfaces_.find(name);
    if (it != interfaces_.end()) {
        return it->second;
    }
    return nullptr;
}

interface_i* node_i::get_prepared_interface(node_map_t& nodes, node_state& state, std::string_view name)
{
    auto* iface = find_interface(name);
    if (iface == nullptr) {
        return nullptr;
    }

    if (!iface->has_value()) {
        execute(nodes, state);
    }

    if (!iface->has_value()) {
        throw std::runtime_error("interface does not contain value after execute");
    }

    return iface;
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
#if 0
            error = error_e::invalid_type;
            return nullptr;
#else
            return dummy::create_node(type);
#endif
    }
}

} // namespace miximus::nodes