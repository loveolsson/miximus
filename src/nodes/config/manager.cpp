#include "nodes/config/manager.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "utils/bind.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes {
using nlohmann::json;

static auto log() { return spdlog::get("http"); };

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
    log()->info(R"(Creating "{}" node with id "{}")", type_to_string(type), id);

    if (config_.find_node(id) != nullptr) {
        log()->warn(R"(Node ID "{}" already in use)", id);
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

    log()->info(R"(Removing node with id "{}")", id);

    auto* node = config_.find_node(id);
    if (node == nullptr) {
        log()->warn("Failed to remove node", id);
        return error_e::not_found;
    }

    const auto& ifaces = node->get_interfaces();
    for (const auto& [_, iface] : ifaces) {
        const auto& cons = iface->get_connections();
        removed_connections.insert(removed_connections.end(), cons.begin(), cons.end());
    }

    for (const auto& rcon : removed_connections) {
        handle_remove_connection(rcon, client_id, false);
    }

    for (auto& adapter : adapters_) {
        adapter->emit_remove_node(id, client_id);
    }

    config_.nodes.erase(id);

    return error_e::no_error;
}

error_e node_manager::handle_update_node(const std::string& id, const nlohmann::json& options, int64_t client_id)
{
    std::unique_lock lock(nodes_mutex_);

    log()->info(R"(Updating node with id "{}")", id);

    auto* node = config_.find_node(id);
    if (node == nullptr) {
        log()->warn(R"(Node with id "{}" not found)", id);
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

static bool is_connection_circular(const node_cfg&             cfg,
                                   std::set<std::string_view>* cleared_nodes,
                                   std::string_view            target_node_id,
                                   const connection&           con)
{
    using dir = interface::dir;

    if (cleared_nodes->count(con.from_node) > 0) {
        return false;
    }

    if (con.from_node == target_node_id) {
        return true;
    }

    if (const auto* node_ = cfg.find_node(con.from_node)) {
        for (const auto& [_, iface] : node_->get_interfaces()) {
            if (iface->direction() == dir::output) {
                continue;
            }

            for (const auto& c : iface->get_connections()) {
                if (is_connection_circular(cfg, cleared_nodes, target_node_id, c)) {
                    return true;
                }
            }
        }
    }

    cleared_nodes->emplace(con.from_node);
    return false;
}

error_e node_manager::handle_add_connection(connection con, int64_t client_id)
{
    using dir = interface::dir;
    std::unique_lock lock(nodes_mutex_);

    if (config_.connections.count(con) > 0) {
        return error_e::duplicate_id;
    }

    auto* from_node = config_.find_node(con.from_node);
    auto* to_node   = config_.find_node(con.to_node);

    if (from_node == nullptr || to_node == nullptr) {
        log()->warn("Node pair not found: {}, {}", con.from_node, con.to_node);
        return error_e::not_found;
    }

    auto* from_iface = from_node->find_interface(con.from_interface);
    auto* to_iface   = to_node->find_interface(con.to_interface);

    if (from_iface == nullptr || to_iface == nullptr) {
        log()->warn("Interface pair not found: {}, {}", con.from_interface, con.to_interface);
        return error_e::not_found;
    }

    auto from_dir = from_iface->direction();
    auto to_dir   = to_iface->direction();

    if (from_dir == dir::input && to_dir == dir::output) {
        // Handle the case where the connection has been declared to-from
        std::swap(con.from_node, con.to_node);
        std::swap(con.from_interface, con.to_interface);
        std::swap(from_iface, to_iface);

        // Re-check duplication after swap
        if (config_.connections.count(con) > 0) {
            return error_e::duplicate_id;
        }
    } else if (from_dir == dir::input || to_dir == dir::output) {
        log()->warn("Interface directions does not match: {}, {}", from_dir, to_dir);
        return error_e::invalid_type;
    }

    if (!test_interface_pair(from_iface->type(), to_iface->type())) {
        log()->warn("Interface types does not match: {}, {}", from_iface->type(), to_iface->type());
        return error_e::invalid_type;
    }

    std::set<std::string_view> cleared_nodes;
    if (is_connection_circular(config_, &cleared_nodes, con.to_node, con)) {
        log()->warn("Connection is circular");
        return error_e::circular_connection;
    }

    con_set_t removed_connections;
    config_.connections.emplace(con);
    from_iface->add_connection(con, removed_connections);
    to_iface->add_connection(con, removed_connections);

    for (const auto& rcon : removed_connections) {
        handle_remove_connection(rcon, client_id, false);
    }

    for (auto& adapter : adapters_) {
        adapter->emit_add_connection(con, client_id);
    }

    return error_e::no_error;
}

error_e node_manager::handle_remove_connection(const connection& con, int64_t client_id, bool do_lock)
{
    auto lock = do_lock ? std::unique_lock<std::mutex>(nodes_mutex_) : std::unique_lock<std::mutex>();

    auto remove_from_interface = [&](const auto& node_name, const auto& iface_name) {
        if (auto node_ = config_.find_node(node_name)) {
            if (auto iface = node_->find_interface(iface_name)) {
                return iface->remove_connection(con);
            }
        }

        return false;
    };

    remove_from_interface(con.from_node, con.from_interface);
    remove_from_interface(con.to_node, con.to_interface);

    if (config_.connections.erase(con) > 0) {
        for (auto& adapter : adapters_) {
            adapter->emit_remove_connection(con, client_id);
        }

        return error_e::no_error;
    }

    return error_e::not_found;
}

json node_manager::get_config()
{
    using nlohmann::json;

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

void node_manager::clear_adapters()
{
    std::unique_lock lock(nodes_mutex_);
    adapters_.clear();
}

} // namespace miximus::nodes