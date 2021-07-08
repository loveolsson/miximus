#include "nodes/node.hpp"
#include "logger/logger.hpp"
#include "nodes/dummy/dummy.hpp"
#include "nodes/interface.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes {

node::node()
{
    options_.emplace("position", &opt_position_);
    options_.emplace("name", &opt_name_);
}

void node::complete()
{
    for (auto& [name, iface] : interfaces_) {
        (void)name;
        iface->reset();
    }
}

bool node::set_option(std::string_view option, const nlohmann::json& value)
{
    auto it = options_.find(option);

    if (it != options_.end()) {
        return it->second->set_json(value);
    }

    return false;
}

nlohmann::json node::get_options()
{
    auto options = nlohmann::json::object();

    for (auto& [name, option] : options_) {
        options.emplace(name, option->get_json());
    }

    return options;
}

nlohmann::json node::get_option(std::string_view option)
{
    auto it = options_.find(option);
    if (it != options_.end()) {
        return it->second->get_json();
    }

    return nlohmann::json(); // null
}

interface* node::find_interface(std::string_view name)
{
    auto it = interfaces_.find(name);
    if (it != interfaces_.end()) {
        return it->second;
    }
    return nullptr;
}

interface* node::get_prepared_interface(const node_cfg& cfg, std::string_view name)
{
    auto* iface = find_interface(name);
    if (iface == nullptr) {
        return nullptr;
    }

    if (!iface->has_value()) {
        execute(cfg);
    }

    if (!iface->has_value()) {
        throw std::runtime_error("interface does not contain value after execute");
    }

    return iface;
}

std::shared_ptr<node> create_node(node_type_e type, message::error_e& error)
{
    switch (type) {
        case node_type_e::decklink_producer:
            error = message::error_e::invalid_type;
            return nullptr;
        case node_type_e::decklink_consumer:
            error = message::error_e::invalid_type;
            return nullptr;

        default:
#if 1
            error = message::error_e::invalid_type;
            return nullptr;
#else
            return dummy::create_node();
#endif
    }
}

} // namespace miximus::nodes