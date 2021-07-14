#include "nodes/config/manager.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "utils/bind.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes {
using nlohmann::json;

static auto log() { return spdlog::get("app"); };

node_map_t node_manager::clone_nodes()
{
    std::unique_lock lock(nodes_mutex_);
    return nodes_;
}

error_e
node_manager::handle_add_node(node_type_e type, const std::string& id, const nlohmann::json& options, int64_t client_id)
{
    std::unique_lock lock(nodes_mutex_);
    log()->info("Creating {} node with id {}", type_to_string(type), id);

    if (nodes_.count(id) > 0) {
        log()->warn("Node id {} already in use", id);
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

    auto resolved_options = node->get_options();

    for (auto& adapter : adapters_) {
        adapter->emit_add_node(type, id, resolved_options, client_id);
    }

    node_record record;

    // Prime the state with a con_set_t for each interface
    for (auto& [id, _] : node->get_interfaces()) {
        record.state.con_map.emplace(id, con_set_t{});
    }

    record.node = std::move(node);
    nodes_.emplace(id, std::move(record));

    return error;
}

error_e node_manager::handle_remove_node(const std::string& id, int64_t client_id)
{
    std::unique_lock lock(nodes_mutex_);

    std::vector<connection> removed_connections;

    log()->info("Removing node with id {}", id);

    auto node_it = nodes_.find(id);
    if (node_it == nodes_.end()) {
        log()->warn("Node with id {} not found", id);
        return error_e::not_found;
    }

    auto& node = node_it->second.node;

    const auto& ifaces = node->get_interfaces();
    for (const auto& [id, iface] : ifaces) {
        const auto& cons = node_it->second.state.con_map[id];
        removed_connections.insert(removed_connections.end(), cons.begin(), cons.end());
    }

    for (const auto& rcon : removed_connections) {
        handle_remove_connection(rcon, client_id, false);
    }

    for (auto& adapter : adapters_) {
        adapter->emit_remove_node(id, client_id);
    }

    nodes_.erase(node_it);

    return error_e::no_error;
}

error_e node_manager::handle_update_node(const std::string& id, const nlohmann::json& options, int64_t client_id)
{
    std::unique_lock lock(nodes_mutex_);

    log()->info("Updating node with id {}", id);

    auto node_it = nodes_.find(id);
    if (node_it == nodes_.end()) {
        log()->warn("Node with id {} not found", id);
        return error_e::not_found;
    }

    auto& node = node_it->second.node;

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

static bool is_connection_circular(const node_map_t&           nodes,
                                   std::set<std::string_view>* cleared_nodes,
                                   std::string_view            target_node_id,
                                   const connection&           con)
{
    using dir = interface_i::dir;

    if (cleared_nodes->count(con.from_node) > 0) {
        return false;
    }

    if (con.from_node == target_node_id) {
        return true;
    }

    auto node_it = nodes.find(con.from_node);
    if (node_it != nodes.end()) {
        auto& node    = node_it->second.node;
        auto& con_map = node_it->second.state.con_map;

        for (const auto& [id, iface] : node->get_interfaces()) {
            if (iface->direction() == dir::output) {
                continue;
            }

            auto connections = con_map.find(id);
            assert(connections != con_map.end());

            for (const auto& c : connections->second) {
                if (is_connection_circular(nodes, cleared_nodes, target_node_id, c)) {
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
    using dir = interface_i::dir;
    std::unique_lock lock(nodes_mutex_);

    if (connections_.count(con) > 0) {
        return error_e::duplicate_id;
    }

    auto from_node_it = nodes_.find(con.from_node);
    auto to_node_it   = nodes_.find(con.to_node);

    if (from_node_it == nodes_.end() || to_node_it == nodes_.end()) {
        log()->warn("Node pair not found: {}, {}", con.from_node, con.to_node);
        return error_e::not_found;
    }

    auto& from_node = from_node_it->second.node;
    auto& to_node   = to_node_it->second.node;

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
        std::swap(from_node_it, to_node_it);

        // Re-check duplication after swap
        if (connections_.count(con) > 0) {
            return error_e::duplicate_id;
        }
    } else if (from_dir == dir::input || to_dir == dir::output) {
        log()->warn("Interface directions does not match: {}, {}", from_dir, to_dir);
        return error_e::invalid_type;
    }

    if (!to_iface->accepts(from_iface->type())) {
        log()->warn("Interface types does not match: {}, {}", from_iface->type(), to_iface->type());
        return error_e::invalid_type;
    }

    std::set<std::string_view> cleared_nodes;
    if (is_connection_circular(nodes_, &cleared_nodes, con.to_node, con)) {
        log()->warn("Attempted connection is circular");
        return error_e::circular_connection;
    }

    con_set_t removed_connections;
    connections_.emplace(con);

    auto& from_connections = from_node_it->second.state.con_map[con.from_interface];
    auto& to_connections   = to_node_it->second.state.con_map[con.to_interface];

    from_iface->add_connection(&from_connections, con, removed_connections);
    to_iface->add_connection(&to_connections, con, removed_connections);

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
        auto node_it = nodes_.find(node_name);
        if (node_it != nodes_.end()) {
            auto& node  = node_it->second.node;
            auto& state = node_it->second.state;

            auto connections = state.con_map.find(iface_name);
            if (connections != state.con_map.end()) {
                connections->second.erase(con);
            }
        }

        return false;
    };

    remove_from_interface(con.from_node, con.from_interface);
    remove_from_interface(con.to_node, con.to_interface);

    if (connections_.erase(con) > 0) {
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

    for (const auto& [id, state] : nodes_) {
        json cfg{
            {"id", id},
            {"type", type_to_string(state.node->type())},
            {"options", state.node->get_options()},
        };

        nodes.emplace_back(std::move(cfg));
    }

    for (const auto& con : connections_) {
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

void node_manager::set_config(const nlohmann::json& settings)
{
    const auto& nodes       = settings["nodes"];
    const auto& connections = settings["connections"];

    for (const auto& node_obj : nodes) {
        std::string type    = node_obj["type"];
        std::string id      = node_obj["id"];
        const auto& options = node_obj["options"];

        handle_add_node(type_from_string(type), id, options, -1);
    }

    for (const auto& con_obj : connections) {
        connection con;
        con.from_node      = con_obj["from_node"];
        con.from_interface = con_obj["from_interface"];
        con.to_node        = con_obj["to_node"];
        con.to_interface   = con_obj["to_interface"];

        handle_add_connection(con, -1);
    }
}

void node_manager::add_adapter(std::unique_ptr<adapter_i>&& adapter)
{
    std::unique_lock lock(nodes_mutex_);
    if (adapter) {
        adapters_.emplace_back(std::move(adapter));
    }
}

void node_manager::clear_adapters()
{
    std::unique_lock lock(nodes_mutex_);
    adapters_.clear();
}

void node_manager::run_one_frame()
{
    auto conf = clone_nodes();

    std::vector<node_record*> consumers;

    for (auto& [_, state] : conf) {
        if (state.node->prepare()) {
            consumers.emplace_back(&state);
        }
    }

    for (auto* record : consumers) {
        record->node->execute(conf, record->state);
    }

    for (auto& [_, state] : conf) {
        state.node->complete();
    }
}

} // namespace miximus::nodes