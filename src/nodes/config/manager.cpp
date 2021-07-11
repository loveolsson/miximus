#include "nodes/config/manager.hpp"
#include "logger/logger.hpp"
#include "nodes/node.hpp"
#include "utils/bind.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes {
using nlohmann::json;
using namespace message;

node_manager::node_manager() {}

node_manager::~node_manager() { adapters_.clear(); }

node_cfg node_manager::clone_node_config()
{
    std::unique_lock lock(nodes_mutex_);

    auto clone = config_;
    return clone;
}

error_e
node_manager::handle_add_node(node_type_e type, const std::string& id, const nlohmann::json& options, int64_t client_id)
{
    std::unique_lock lock(nodes_mutex_);
    spdlog::get("http")->info(R"(Creating "{}" node with id "{}")", type, id);

    if (config_.find_node(id) != nullptr) {
        spdlog::get("http")->warn(R"(Node ID "{}" already in use)", id);
        return error_e::duplicate_id;
    }

    error_e error = error_e::no_error;
    auto    node  = create_node(type, error);

    if (error != error_e::no_error || !node) {
        return error;
    }

    for (auto option = options.begin(); option != options.end(); ++option) {
        node->set_option(option.key(), option.value());
    }

    config_.nodes.emplace(id, node);

    auto resolved_options = node->get_options();
    for (auto& adapter : adapters_) {
        adapter->emit_add_node(type, id, resolved_options, client_id);
    }

    return error;
}

error_e node_manager::handle_remove_node(const std::string& id, int64_t client_id)
{
    std::unique_lock lock(nodes_mutex_);

    std::vector<connection> removed_connections;

    spdlog::get("http")->info(R"(Removing node with id "{}")", id);

    bool res = config_.erase_node(id, removed_connections);
    if (!res) {
        spdlog::get("http")->warn("Failed to remove node", id);
        return error_e::not_found;
    }

    for (auto& connection : removed_connections) {
        for (auto& adapter : adapters_) {
            adapter->emit_remove_connection(connection, client_id);
        }
    }

    for (auto& adapter : adapters_) {
        adapter->emit_remove_node(id, client_id);
    }

    return error_e::no_error;
}

error_e node_manager::handle_update_node(const std::string& id, const nlohmann::json& options, int64_t client_id)
{
    std::unique_lock lock(nodes_mutex_);

    spdlog::get("http")->info(R"(Updating node with id "{}")", id);

    auto* node = config_.find_node(id);
    if (node == nullptr) {
        spdlog::get("http")->warn(R"(Node with id "{}" not found)", id);
        return error_e::not_found;
    }

    auto bcast_options = nlohmann::json::object();

    for (auto option = options.begin(); option != options.end(); ++option) {
        if (node->set_option(option.key(), option.value())) {
            bcast_options[option.key()] = option.value();
        }
    }

    for (auto& adapter : adapters_) {
        adapter->emit_update_node(id, bcast_options, client_id);
    }

    return error_e::no_error;
}

error_e node_manager::handle_add_connection(const connection& con, int64_t client_id)
{
    std::unique_lock lock(nodes_mutex_);
    return error_e::no_error;
}

error_e node_manager::handle_remove_connection(const connection& con, int64_t client_id)
{
    std::unique_lock lock(nodes_mutex_);
    return error_e::no_error;
}

json node_manager::get_config()
{
    using namespace nlohmann;
    using namespace message;

    std::unique_lock lock(nodes_mutex_);

    auto nodes       = json::array();
    auto connections = json::array();

    for (const auto& [id, node] : config_.nodes) {
        json cfg{
            {"id", id},
            {"type", type_to_string(node->type())},
            {"options", node->get_options()},
        };

        nodes.emplace_back(std::move(cfg));
    }

    for (const auto& con : config_.connections) {
        json cfg{
            {"from_node", con.from_node},
            {"from_interface", con.from_interface},
            {"to_node", con.to_node},
            {"to_interface", con.to_interface},
        };

        connections.emplace_back(std::move(cfg));
    }

    return {
        {"nodes", std::move(nodes)},
        {"connections", std::move(connections)},
    };
}

void node_manager::add_adapter(std::unique_ptr<config_adapter>&& adapter)
{
    std::unique_lock lock(nodes_mutex_);
    if (adapter) {
        adapter->set_manager(this);
        adapters_.emplace_back(std::move(adapter));
    }
}

} // namespace miximus::nodes