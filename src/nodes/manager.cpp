#include "nodes/manager.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "utils/bind.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json.hpp>

#include <set>

namespace miximus::nodes {
using nlohmann::json;

static auto log() { return spdlog::get("app"); };

static bool is_valid_common_option(std::string_view name, const json& value)
{
    if (name == "position") {
        if (value.is_array() && value.size() == 2 && value[0].is_number() && value[1].is_number()) {
            return true;
        }
    } else if (name == "name") {
        if (value.is_string()) {
            return true;
        }
    }
    return false;
}

error_e node_manager_s::handle_add_node(node_type_e           type,
                                        const std::string&    id,
                                        const nlohmann::json& options,
                                        int64_t               client_id)
{
    std::unique_lock lock(nodes_mutex_);
    log()->info("Creating {} node with id {}", type_to_string(type), id);

    if (nodes_.count(id) > 0) {
        log()->warn("Node id {} already in use", id);
        return error_e::duplicate_id;
    }

    error_e error = error_e::no_error;
    auto    node  = create_node(type, error);

    if (error != error_e::no_error) {
        return error;
    }

    node_record_s record;
    record.state.options = node->get_default_options();

    for (auto option = options.begin(); option != options.end(); ++option) {
        const auto& key   = option.key();
        const auto& value = option.value();

        if (is_valid_common_option(key, value) || node->check_option(key, value)) {
            record.state.options[key] = value;
        }
    }

    for (auto& adapter : adapters_) {
        adapter->emit_add_node(type, id, record.state.options, client_id);
    }

    // Prime the state with a con_set_t for each interface
    for (const auto& [id, _] : node->get_interfaces()) {
        record.state.con_map.emplace(id, con_set_t{});
    }

    record.node = std::move(node);
    nodes_.emplace(id, std::move(record));

    return error;
}

error_e node_manager_s::handle_remove_node(const std::string& id, int64_t client_id)
{
    std::unique_lock lock(nodes_mutex_);

    std::vector<connection_s> removed_connections;

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

error_e node_manager_s::handle_update_node(const std::string& id, const nlohmann::json& options, int64_t client_id)
{
    std::unique_lock lock(nodes_mutex_);

    log()->info("Updating node with id {}", id);

    auto node_it = nodes_.find(id);
    if (node_it == nodes_.end()) {
        log()->warn("Node with id {} not found", id);
        return error_e::not_found;
    }

    auto& node  = node_it->second.node;
    auto& state = node_it->second.state;

    for (auto option = options.begin(); option != options.end(); ++option) {
        const auto& key   = option.key();
        const auto& value = option.value();

        if (is_valid_common_option(key, value) || node->check_option(key, value)) {
            state.options[key] = value;
        }
    }

    for (auto& adapter : adapters_) {
        adapter->emit_update_node(id, state.options, client_id);
    }

    return error_e::no_error;
}

static bool is_connection_circular(node_map_t&                 nodes,
                                   std::set<std::string_view>* cleared_nodes,
                                   std::string_view            target_node_id,
                                   const connection_s&         con)
{
    using dir_e = interface_i::dir_e;

    if (cleared_nodes->count(con.from_node) > 0) {
        return false;
    }

    if (con.from_node == target_node_id) {
        return true;
    }

    if (auto node_it = nodes.find(con.from_node); node_it != nodes.end()) {
        const auto& node    = node_it->second.node;
        const auto& con_map = node_it->second.state.con_map;

        for (const auto& [id, iface] : node->get_interfaces()) {
            if (iface->direction() == dir_e::output) {
                continue;
            }

            auto connections = con_map.find(id);
            if (connections == con_map.end()) {
                assert(false);
                continue;
            }

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

error_e node_manager_s::handle_add_connection(connection_s con, int64_t client_id)
{
    using dir_e = interface_i::dir_e;
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

    if (from_dir == dir_e::input && to_dir == dir_e::output) {
        // Handle the case where the connection has been declared to-from
        std::swap(con.from_node, con.to_node);
        std::swap(con.from_interface, con.to_interface);
        std::swap(from_iface, to_iface);
        std::swap(from_node_it, to_node_it);

        // Re-check duplication after swap
        if (connections_.count(con) > 0) {
            return error_e::duplicate_id;
        }
    } else if (from_dir == dir_e::input || to_dir == dir_e::output) {
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

error_e node_manager_s::handle_remove_connection(const connection_s& con, int64_t client_id, bool do_lock)
{
    auto lock = do_lock ? std::unique_lock<std::mutex>(nodes_mutex_) : std::unique_lock<std::mutex>();

    auto remove_from_interface = [&](const auto& node_name, const auto& iface_name) {
        if (auto node_it = nodes_.find(node_name); node_it != nodes_.end()) {
            auto& node  = node_it->second.node;
            auto& state = node_it->second.state;

            if (auto cons_it = state.con_map.find(iface_name); cons_it != state.con_map.end()) {
                cons_it->second.erase(con);
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

json node_manager_s::get_config()
{
    std::unique_lock lock(nodes_mutex_);

    auto nodes       = json::array();
    auto connections = json::array();

    for (const auto& [id, record] : nodes_) {
        nodes.emplace_back(json{
            {"id", id},
            {"type", type_to_string(record.node->type())},
            {"options", record.state.options},
        });
    }

    for (const auto& con : connections_) {
        json j = con;

        connections.emplace_back(std::move(j));
    }

    return {
        {"nodes", std::move(nodes)},
        {"connections", std::move(connections)},
    };
}

void node_manager_s::set_config(const nlohmann::json& settings)
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
        auto con = con_obj.get<connection_s>();

        handle_add_connection(con, -1);
    }
}

void node_manager_s::add_adapter(std::unique_ptr<adapter_i>&& adapter)
{
    std::unique_lock lock(nodes_mutex_);
    if (adapter) {
        adapters_.emplace_back(std::move(adapter));
    }
}

void node_manager_s::clear_adapters()
{
    std::unique_lock lock(nodes_mutex_);
    adapters_.clear();
}

void node_manager_s::tick_one_frame(app_state_s& app)
{
    {
        /**
         * A few things are accomplished by copying the node map here:
         * - The config if fixed during this frame, while not locking up the config thread
         * - The lifetime of a node is guaraneed for the duration of the frame
         * - Any node that has been prepared at least once is guaranteed to be destroyed in this thread,
         *      making resource management a lot simpler
         */
        std::unique_lock lock(nodes_mutex_);
        nodes_copy_ = nodes_;
    }

    std::vector<node_record_s*> must_execute;

    // Call prepare on all nodes, and collect the ones that must execute
    for (auto& [_, record] : nodes_copy_) {
        if (record.node->prepare(app, record.state)) {
            must_execute.emplace_back(&record);
        }
    }

    for (auto* record : must_execute) {
        if (!record->state.executed) {
            record->node->execute(app, nodes_copy_, record->state);
        }
    }

    for (auto& [_, record] : nodes_copy_) {
        record.node->complete();
    }
}

} // namespace miximus::nodes