#include "core/node_manager.hpp"

#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "gpu/context.hpp"
#include "gpu/sync.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/validate_option.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace {
auto _log() { return getlog("app"); };

bool is_connection_circular(const miximus::nodes::node_map_t&   nodes,
                            std::string_view                    target_node_id,
                            const miximus::nodes::connection_s& initial_con)
{
    std::vector<std::string_view> stack;
    std::set<std::string_view>    visited;

    stack.push_back(initial_con.from_node);

    while (!stack.empty()) {
        const auto node_id = stack.back();
        stack.pop_back();

        if (!visited.emplace(node_id).second) {
            continue;
        }

        if (node_id == target_node_id) {
            return true;
        }

        const auto node_it = nodes.find(std::string(node_id));
        if (node_it == nodes.end()) {
            continue;
        }

        const auto& node    = node_it->second.node;
        const auto& con_map = node_it->second.state.con_map;

        for (const auto& [id, iface] : node->get_interfaces()) {
            using dir_e = miximus::nodes::interface_i::dir_e;
            if (iface->direction() == dir_e::output) {
                continue;
            }

            if (const auto it = con_map.find(id); it != con_map.end()) {
                for (const auto& c : it->second) {
                    if (!visited.contains(c.from_node)) {
                        stack.push_back(c.from_node);
                    }
                }
            }
        }
    }

    return false;
}
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

    node->init(id);

    nodes::node_record_s record;
    record.state.options = node->get_default_options();
    if (const auto e = node->set_options(record.state.options, options); e != error_e::no_error) {
        return e;
    }

    for (auto& adapter : adapters_) {
        adapter->emit_add_node(type, id, record.state.options, client_id);
    }

    // Prime the state with a con_set_t for each interface
    for (const auto& [iface_id, _] : node->get_interfaces()) {
        record.state.con_map.emplace(iface_id, nodes::con_set_t{});
    }

    record.node = std::move(node);
    nodes_.emplace(id, std::move(record));
    dirty_nodes_.emplace(id_str);

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
    for (const auto& [iface_id, iface] : ifaces) {
        const auto& cons = node_it->second.state.con_map.at(iface_id);
        removed_connections.insert(removed_connections.end(), cons.begin(), cons.end());
    }

    for (const auto& rcon : removed_connections) {
        remove_connection_locked(rcon, client_id);
    }

    for (auto& adapter : adapters_) {
        adapter->emit_remove_node(id, client_id);
    }

    removed_nodes_.emplace(node_it->first);
    nodes_.erase(node_it);

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

    dirty_nodes_.emplace(id_str);

    return error_e::no_error;
}

error_e node_manager_s::handle_add_connection(nodes::connection_s con, int64_t client_id)
{
    using dir_e = nodes::interface_i::dir_e;
    const std::unique_lock lock(nodes_mutex_);

    _log()->info(
        "Adding connection between {}:{}, {}:{}", con.from_node, con.from_interface, con.to_node, con.to_interface);

    if (std::ranges::find(connections_, con) != connections_.end()) {
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
        if (std::ranges::find(connections_, con) != connections_.end()) {
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

    if (is_connection_circular(nodes_, con.to_node, con)) {
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
        remove_connection_locked(rcon, client_id);
    }

    for (auto& adapter : adapters_) {
        adapter->emit_add_connection(con, client_id);
    }

    dirty_nodes_.emplace(con.from_node);
    dirty_nodes_.emplace(con.to_node);

    return error_e::no_error;
}

error_e node_manager_s::remove_connection_locked(const nodes::connection_s& con, int64_t client_id)
{
    _log()->info(
        "Removing connection between {}:{}, {}:{}", con.from_node, con.from_interface, con.to_node, con.to_interface);

    auto con_it = std::ranges::find(connections_, con);
    if (con_it == connections_.end()) {
        return error_e::not_found;
    }

    auto remove_from_interface = [&](const auto& node_name, const auto& iface_name) {
        auto node_it = nodes_.find(node_name);
        if (node_it == nodes_.end()) {
            _log()->error("Node {} not found when removing connection, lists are out of sync", node_name);
            return;
        }

        auto& con_map = node_it->second.state.con_map;
        auto  cons_it = con_map.find(iface_name);
        if (cons_it == con_map.end()) {
            _log()->error("Interface {} on node {} not found when removing connection, lists are out of sync",
                          iface_name,
                          node_name);
            return;
        }

        const auto removed_count = std::erase(cons_it->second, con);
        if (removed_count != 1) {
            _log()->error("Connection not found in node {} interface {}, lists are out of sync", node_name, iface_name);
        }
    };

    remove_from_interface(con.from_node, con.from_interface);
    remove_from_interface(con.to_node, con.to_interface);

    for (auto& adapter : adapters_) {
        adapter->emit_remove_connection(con, client_id);
    }

    connections_.erase(con_it);

    dirty_nodes_.emplace(con.from_node);
    dirty_nodes_.emplace(con.to_node);

    return error_e::no_error;
}

error_e node_manager_s::handle_remove_connection(const nodes::connection_s& con, int64_t client_id)
{
    const std::unique_lock lock(nodes_mutex_);
    return remove_connection_locked(con, client_id);
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

    nlohmann::json status = nlohmann::json::object();
    if (status_registry_) {
        status = status_registry_->get_all();
    }

    return {
        {"nodes",       std::move(nodes)      },
        {"connections", std::move(connections)},
        {"status",      std::move(status)     },
    };
}

nlohmann::json node_manager_s::get_node_status(std::string_view id) const
{
    if (status_registry_ == nullptr) {
        return nlohmann::json::object();
    }
    return status_registry_->get(id);
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
    status_registry_ = app->status_registry();

    app->ctx()->make_current();

    {
        /**
         * A few things are accomplished by copying the node map here:
         * - The config if fixed during this frame, while not locking up the config thread
         * - The lifetime of a node is guaraneed for the duration of the frame
         * - Any node that has been prepared at least once is guaranteed to be destroyed in this thread,
         *      making resource management a lot simpler
         */
        std::unique_lock lock(nodes_mutex_);
        if (!dirty_nodes_.empty() || !removed_nodes_.empty()) {
            for (const auto& id : removed_nodes_) {
                nodes_copy_.erase(id);
                app->status_registry()->remove_node(id);
            }
            for (const auto& id : dirty_nodes_) {
                if (auto it = nodes_.find(id); it != nodes_.end()) {
                    nodes_copy_.insert_or_assign(it->first, it->second);
                }
            }
            _log()->info("Updated render graph: {} changed, {} removed", dirty_nodes_.size(), removed_nodes_.size());
            dirty_nodes_.clear();
            removed_nodes_.clear();
        } else {
            lock.unlock();
        }
    }

    app->frame_info.executed_nodes.clear();

    std::vector<std::pair<std::string_view, nodes::node_record_s*>> must_execute;

    // Call prepare on all nodes, and collect their traits
    for (auto& [id, record] : nodes_copy_) {
        nodes::node_i::traits_s traits = {};
        record.node->prepare(app, record.state, &traits);

        if (traits.must_run) {
            must_execute.emplace_back(id, &record);
        }
    }

    for (auto [id, record] : must_execute) {
        if (!app->frame_info.executed_nodes.contains(id)) {
            app->frame_info.executed_nodes.emplace(id);
            record->node->execute(app, nodes_copy_, record->state);
        }
    }

    gpu::context_s::finish();

    for (auto& [_, record] : nodes_copy_) {
        record.node->complete(app);
    }

    gpu::context_s::rewind_current();

    // Broadcast status changes collected during this tick
    const auto changed_ids = app->status_registry()->flush();
    for (const auto& id : changed_ids) {
        const auto status = app->status_registry()->get(id);
        for (auto& adapter : adapters_) {
            adapter->emit_node_status(id, status);
        }
    }
}

void node_manager_s::clear_nodes(app_state_s* app)
{
    app->ctx()->make_current();

    nodes_copy_.clear();
    nodes_.clear();
    connections_.clear();
    dirty_nodes_.clear();
    removed_nodes_.clear();

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