#include "core/node_manager.hpp"
#include "core/app_state.hpp"
#include "gpu/context.hpp"
#include "gpu/sync.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/validate_option.hpp"
#include "utils/bind.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json.hpp>

#include <set>

namespace {
auto _log() { return getlog("app"); };
} // namespace

namespace miximus::core {
using nlohmann::json;
using namespace std::chrono_literals;

node_manager_s::node_manager_s() { nodes::register_all_nodes(&constructors_); }

error_e
node_manager_s::handle_add_node(std::string_view type, std::string_view id, const json& options, int64_t client_id)
{
    const std::unique_lock lock(nodes_mutex_);
    _log()->info("Creating {} node with id {}", type, id);

    const std::string id_str(id);

    if (nodes_.contains(id_str)) {
        _log()->warn("Node id {} already in use", id);
        return error_e::duplicate_id;
    }

    auto [node, error] = create_node(type);
    if (error != error_e::no_error) {
        return error;
    }

    nodes::node_record_s record;
    record.state.options = node->get_default_options();
    if (const auto e = node->set_options(record.state.options, options); e != error_e::no_error) {
        return e;
    }

    for (auto& adapter : adapters_) {
        adapter->emit_add_node(type, id, record.state.options, client_id);
    }

    // Prime the state with a con_set_t for each interface
    for (const auto& [id, _] : node->get_interfaces()) {
        record.state.con_map.emplace(id, nodes::con_set_t{});
    }

    record.node = std::move(node);
    nodes_.emplace(id, std::move(record));
    nodes_dirty_ = true;

    return error;
}

error_e node_manager_s::handle_remove_node(std::string_view id, int64_t client_id)
{
    const std::unique_lock lock(nodes_mutex_);

    _log()->info("Removing node with id {}", id);

    auto node_it = nodes_.find(std::string(id));
    if (node_it == nodes_.end()) {
        _log()->warn("Node with id {} not found", id);
        return error_e::not_found;
    }

    nodes::con_set_t removed_connections;
    auto&            node = node_it->second.node;

    const auto& ifaces = node->get_interfaces();
    for (const auto& [id, iface] : ifaces) {
        const auto& cons = node_it->second.state.con_map.at(id);
        removed_connections.insert(removed_connections.end(), cons.begin(), cons.end());
    }

    for (const auto& rcon : removed_connections) {
        handle_remove_connection(rcon, client_id);
    }

    for (auto& adapter : adapters_) {
        adapter->emit_remove_node(id, client_id);
    }

    nodes_.erase(node_it);
    nodes_dirty_ = true;

    return error_e::no_error;
}

error_e node_manager_s::handle_update_node(std::string_view id, const json& options, int64_t client_id)
{
    const std::unique_lock lock(nodes_mutex_);

    const std::string id_str(id);

    auto node_it = nodes_.find(id_str);
    if (node_it == nodes_.end()) {
        _log()->warn("Update node: Id {} not found", id);
        return error_e::not_found;
    }

    _log()->info("Updating node with id {}", id);

    auto& node  = node_it->second.node;
    auto& state = node_it->second.state;

    if (const auto e = node->set_options(state.options, options); e != error_e::no_error) {
        return e;
    }

    for (auto& adapter : adapters_) {
        adapter->emit_update_node(id, state.options, client_id);
    }

    nodes_dirty_ = true;

    return error_e::no_error;
}

// NOLINTNEXTLINE(misc-no-recursion, misc-use-anonymous-namespace)
static bool is_connection_circular(const nodes::node_map_t&    nodes,
                                   std::set<std::string_view>* cleared_nodes,
                                   std::string_view            target_node_id,
                                   const nodes::connection_s&  con)
{
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
            using dir_e = nodes::interface_i::dir_e;
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

error_e node_manager_s::handle_add_connection(nodes::connection_s con, int64_t client_id)
{
    using dir_e = nodes::interface_i::dir_e;
    const std::unique_lock lock(nodes_mutex_);

    _log()->info(
        "Adding connection between {}:{}, {}:{}", con.from_node, con.from_interface, con.to_node, con.to_interface);

    if (std::find(connections_.begin(), connections_.end(), con) != connections_.end()) {
        return error_e::duplicate_id;
    }

    auto from_node_it = nodes_.find(con.from_node);
    auto to_node_it   = nodes_.find(con.to_node);

    if (from_node_it == nodes_.end() || to_node_it == nodes_.end()) {
        _log()->warn("Node pair not found: {}, {}", con.from_node, con.to_node);
        return error_e::not_found;
    }

    const auto& from_node = from_node_it->second.node;
    const auto& to_node   = to_node_it->second.node;

    auto from_iface = from_node->find_interface(con.from_interface);
    auto to_iface   = to_node->find_interface(con.to_interface);

    if (from_iface == nullptr || to_iface == nullptr) {
        _log()->warn("Interface pair not found: {}->{}", con.from_interface, con.to_interface);
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
        if (std::find(connections_.begin(), connections_.end(), con) != connections_.end()) {
            return error_e::duplicate_id;
        }
    } else if (from_dir == dir_e::input || to_dir == dir_e::output) {
        _log()->warn("Interface directions does not match: {}->{}", enum_to_string(from_dir), enum_to_string(to_dir));
        return error_e::invalid_type;
    }

    if (!to_iface->accepts(from_iface->type())) {
        _log()->warn("Interface types does not match: {}, {}",
                     enum_to_string(from_iface->type()),
                     enum_to_string(to_iface->type()));
        return error_e::invalid_type;
    }

    std::set<std::string_view> cleared_nodes;
    if (is_connection_circular(nodes_, &cleared_nodes, con.to_node, con)) {
        _log()->warn("Attempted connection is circular");
        return error_e::circular_connection;
    }

    nodes::con_set_t removed_connections;
    connections_.emplace_back(con);

    auto& from_connections = from_node_it->second.state.con_map.at(con.from_interface);
    from_iface->add_connection(&from_connections, con, &removed_connections);

    auto& to_connections = to_node_it->second.state.con_map.at(con.to_interface);
    to_iface->add_connection(&to_connections, con, &removed_connections);

    for (const auto& rcon : removed_connections) {
        handle_remove_connection(rcon, client_id);
    }

    for (auto& adapter : adapters_) {
        adapter->emit_add_connection(con, client_id);
    }

    nodes_dirty_ = true;

    return error_e::no_error;
}

error_e node_manager_s::handle_remove_connection(const nodes::connection_s& con, int64_t client_id)
{
    const std::unique_lock lock(nodes_mutex_);

    _log()->info(
        "Removing connection between {}:{}, {}:{}", con.from_node, con.from_interface, con.to_node, con.to_interface);

    auto con_it = std::find(connections_.begin(), connections_.end(), con);
    if (con_it == connections_.end()) {
        return error_e::not_found;
    }

    auto remove_from_interface = [&](const auto& node_name, const auto& iface_name) {
        if (auto node_it = nodes_.find(node_name); node_it != nodes_.end()) {
            auto& state = node_it->second.state;

            if (auto cons_it = state.con_map.find(iface_name); cons_it != state.con_map.end()) {
                auto con_it = std::find(cons_it->second.begin(), cons_it->second.end(), con);
                if (con_it != cons_it->second.end()) {
                    cons_it->second.erase(con_it);
                }
            }
        }
    };

    remove_from_interface(con.from_node, con.from_interface);
    remove_from_interface(con.to_node, con.to_interface);

    for (auto& adapter : adapters_) {
        adapter->emit_remove_connection(con, client_id);
    }

    connections_.erase(con_it);

    nodes_dirty_ = true;

    return error_e::no_error;
}

json node_manager_s::get_config()
{
    const std::unique_lock lock(nodes_mutex_);

    auto nodes       = json::array();
    auto connections = json::array();

    for (const auto& [id, record] : nodes_) {
        nodes.emplace_back(json{
            {"id",      id                  },
            {"type",    record.node->type() },
            {"options", record.state.options},
        });
    }

    for (const auto& con : connections_) {
        connections.emplace_back(con);
    }

    return {
        {"nodes",       std::move(nodes)      },
        {"connections", std::move(connections)},
    };
}

void node_manager_s::set_config(const json& settings)
{
    const auto& nodes       = settings.at("nodes");
    const auto& connections = settings.at("connections");

    for (const auto& node_obj : nodes) {
        auto        type    = node_obj.at("type").get<std::string_view>();
        auto        id      = node_obj.at("id").get<std::string_view>();
        const auto& options = node_obj.at("options");

        handle_add_node(type, id, options, -1);
    }

    for (const auto& con_obj : connections) {
        auto con = con_obj.get<nodes::connection_s>();

        handle_add_connection(con, -1);
    }
}

void node_manager_s::add_adapter(std::unique_ptr<adapter_i>&& adapter)
{
    const std::unique_lock lock(nodes_mutex_);
    if (adapter) {
        adapters_.emplace_back(std::move(adapter));
    }
}

void node_manager_s::clear_adapters()
{
    const std::unique_lock lock(nodes_mutex_);
    adapters_.clear();
}

void node_manager_s::tick_one_frame(app_state_s* app)
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
        if (nodes_dirty_) {
            nodes_copy_  = nodes_;
            nodes_dirty_ = false;
            _log()->info("Copied node config graph");
        } else {
            lock.unlock();
            for (auto& node : nodes_copy_) {
                node.second.state.executed = false;
            }
        }
    }

    app->ctx()->make_current();

    std::vector<nodes::node_record_s*> must_execute;

    // Call prepare on all nodes, and collect their traits
    for (auto& [_, record] : nodes_copy_) {
        nodes::node_i::traits_s traits = {};
        record.node->prepare(app, record.state, &traits);

        if (traits.must_run) {
            must_execute.emplace_back(&record);
        }
    }

    for (auto record : must_execute) {
        if (!record->state.executed) {
            record->node->execute(app, nodes_copy_, record->state);
        }
    }

    gpu::context_s::finish();

    for (auto& [_, record] : nodes_copy_) {
        record.node->complete(app);
    }

    gpu::context_s::rewind_current();
}

void node_manager_s::clear_nodes(app_state_s* app)
{
    app->ctx()->make_current();

    nodes_copy_.clear();
    nodes_.clear();
    connections_.clear();

    gpu::context_s::rewind_current();
}

std::pair<std::shared_ptr<nodes::node_i>, error_e> node_manager_s::create_node(std::string_view type)
{
    auto it = constructors_.find(type);
    if (it == constructors_.end()) {
        return {nullptr, error_e::invalid_type};
    }

    auto n = it->second();
    if (n->type() != type) {
        return {nullptr, error_e::internal_error};
    }

    return {n, error_e::no_error};
}

} // namespace miximus::core