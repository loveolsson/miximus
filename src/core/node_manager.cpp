#include "core/node_manager.hpp"

#include "core/app_state.hpp"
#include "core/frame_scheduler.hpp"
#include "core/node_status_registry.hpp"
#include "gpu/device.hpp"
#include "gpu/window.hpp"
#include "logger/logger.hpp"
#include "nodes/frame_execution.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/system/register.hpp"
#include "types/node_status_json.hpp"
#include "utils/flicks.hpp"
#include "web_server/server.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
auto _log() { return getlog("app"); };

// Only reply scheduling is shared. Actions know nothing about graph locks or
// batches; handlers can settle them normally while config mutations are locked.
class action_reply_queue_s
{
    using reply_t  = miximus::nodes::action_s::reply_t;
    using result_t = miximus::nodes::action_result_s;
    std::mutex                                mutex_;
    bool                                      open_{};
    std::vector<std::pair<reply_t, result_t>> pending_;
    static void                               deliver(const reply_t& reply, result_t result) noexcept
    {
        try {
            if (reply) {
                reply(std::move(result));
            }
        } catch (...) {
            miximus::logger::log_error_noexcept("app", "Node action reply failed");
        }
    }

  public:
    void push(reply_t reply, result_t result)
    {
        {
            const std::scoped_lock lock(mutex_);
            if (!open_) {
                pending_.emplace_back(std::move(reply), std::move(result));
                return;
            }
        }
        deliver(reply, std::move(result));
    }
    void open() noexcept
    {
        std::vector<std::pair<reply_t, result_t>> pending;
        {
            const std::scoped_lock lock(mutex_);
            open_ = true;
            pending.swap(pending_);
        }
        for (auto& [reply, result] : pending) {
            deliver(reply, std::move(result));
        }
    }
};

bool is_connection_circular(const miximus::nodes::node_map_t& nodes,
                            std::string_view                  target_node_id,
                            const miximus::connection_s&      initial_con)
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

        const auto node_it = nodes.find(node_id);
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

std::string generate_node_id(const miximus::nodes::node_map_t& nodes)
{
    // Keep this fixed across targets. Fifteen bytes fit the std::string SSO
    // storage used by our Clang/libstdc++ and Windows builds.
    constexpr size_t           id_length = 15;
    constexpr std::string_view alphabet  = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    thread_local std::mt19937_64          generator{std::random_device{}()};
    std::uniform_int_distribution<size_t> character(0, alphabet.size() - 1);

    const auto generate = [&] {
        std::string id(id_length, '\0');
        for (auto& value : id) {
            value = alphabet.at(character(generator));
        }
        return id;
    };

    auto id = generate();
    while (nodes.contains(id)) {
        id = generate();
    }

    return id;
}
} // namespace

namespace miximus::core {
using nlohmann::json;
using namespace std::chrono_literals;

node_manager_s::node_manager_s(node_status_registry_s* status_registry)
    : status_registry_(status_registry)
{
    nodes::register_all_nodes(&node_definitions_);
    const auto error =
        handle_add_node(nodes::system::SETTINGS_NODE_TYPE, nodes::system::SETTINGS_NODE_ID, nlohmann::json::object());
    if (error != error_e::no_error) {
        throw std::logic_error("Failed to create the application settings node");
    }
    settings_status_handle_ = nodes_.at(std::string(nodes::system::SETTINGS_NODE_ID)).node->status_handle();
}

error_e node_manager_s::handle_add_node(std::string_view                    type,
                                        std::string_view                    id,
                                        const json&                         options,
                                        const std::optional<origin_info_s>& origin)
{
    const std::unique_lock lock(nodes_mutex_);
    return handle_add_node_locked(type, id, options, origin);
}

error_e
node_manager_s::handle_add_node(std::string_view type, const json& options, const std::optional<origin_info_s>& origin)
{
    const std::unique_lock lock(nodes_mutex_);
    return handle_add_node_locked(type, generate_node_id(nodes_), options, origin);
}

error_e node_manager_s::handle_add_node_locked(std::string_view                    type,
                                               std::string_view                    id,
                                               const json&                         options,
                                               const std::optional<origin_info_s>& origin)
{
    _log()->info("Creating {} node with id {}", type, id);

    const bool is_settings_id   = id == nodes::system::SETTINGS_NODE_ID;
    const bool is_settings_type = type == nodes::system::SETTINGS_NODE_TYPE;
    if (is_settings_id != is_settings_type) {
        _log()->warn("Application settings type and reserved id must be used together");
        return error_e::invalid_type;
    }

    const std::string id_str(id);

    if (nodes_.contains(id)) {
        _log()->warn("Node id {} already in use", id);
        return error_e::duplicate_id;
    }

    auto [node, error] = create_node(type);
    if (error != error_e::no_error) {
        return error;
    }

    node->init(id);

    nodes::node_record_s record;
    record.state.options       = json::object();
    const auto default_options = node->get_default_options();
    const auto default_result  = node->set_options(record.state.options, default_options);
    if (default_result.error != error_e::no_error || default_result.has_corrected_values) {
        _log()->error("Node type {} has invalid or non-canonical default options", type);
        return error_e::internal_error;
    }

    if (const auto result = node->set_options(record.state.options, options); result.error != error_e::no_error) {
        return result.error;
    }

    // Prime the state with a con_set_t for each interface
    for (const auto& [iface_id, _] : node->get_interfaces()) {
        record.state.con_map.emplace(iface_id, nodes::con_set_t{});
    }

    record.node                    = std::move(node);
    const auto [node_it, inserted] = nodes_.emplace(id, std::move(record));
    assert(inserted);
    pending_nodes_.try_emplace(id_str);

    for (auto& adapter : adapters_) {
        adapter->emit_add_node(type, id, node_it->second.state.options, origin);
    }

    return error;
}

error_e node_manager_s::handle_remove_node(std::string_view id, const std::optional<origin_info_s>& origin)
{
    std::deque<node_actions_s::request_s> cancelled;
    std::unique_lock                      lock(nodes_mutex_);

    if (id == nodes::system::SETTINGS_NODE_ID) {
        return error_e::invalid_type;
    }

    _log()->info("Removing node with id {}", id);

    auto node_it = nodes_.find(id);
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
        remove_connection_locked(rcon, origin);
    }

    for (auto& adapter : adapters_) {
        adapter->emit_remove_node(id, origin);
    }

    if (status_registry_ != nullptr) {
        status_registry_->remove_node(node->status_handle());
    } else {
        node->status_handle().retire();
    }
    removed_nodes_.emplace(node_it->first);
    if (const auto pending = pending_nodes_.find(id); pending != pending_nodes_.end()) {
        cancelled.swap(pending->second.actions);
        pending_nodes_.erase(pending);
    }
    nodes_.erase(node_it);
    lock.unlock();
    for (auto& action : cancelled) {
        action.fail(error_e::not_found, "Target node was removed before frame delivery");
    }

    return error_e::no_error;
}

nodes::set_options_result_s
node_manager_s::handle_update_node(std::string_view id, const json& options, const std::optional<origin_info_s>& origin)
{
    const option_update_s update{.id = std::string(id), .options = options};
    return handle_control_batch(nullptr, std::span(&update, 1), {}, origin);
}

error_e node_manager_s::handle_add_connection(connection_s con, const std::optional<origin_info_s>& origin)
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
        remove_connection_locked(rcon, origin);
    }

    for (auto& adapter : adapters_) {
        adapter->emit_add_connection(con, origin);
    }

    pending_nodes_.try_emplace(con.from_node);
    pending_nodes_.try_emplace(con.to_node);

    return error_e::no_error;
}

error_e node_manager_s::remove_connection_locked(const connection_s& con, const std::optional<origin_info_s>& origin)
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
        adapter->emit_remove_connection(con, origin);
    }

    connections_.erase(con_it);

    pending_nodes_.try_emplace(con.from_node);
    pending_nodes_.try_emplace(con.to_node);

    return error_e::no_error;
}

error_e node_manager_s::handle_remove_connection(const connection_s& con, const std::optional<origin_info_s>& origin)
{
    const std::unique_lock lock(nodes_mutex_);
    return remove_connection_locked(con, origin);
}

void node_manager_s::action_work_s::run(app_state_s* app)
{
    if (!action) {
        return;
    }
    if (!start) {
        action.fail(error_e::internal_error, "Node action was not consumed");
        return;
    }
    try {
        start(app, std::move(action));
    } catch (const std::exception& error) {
        logger::log_error_noexcept("app", "Node configuration action failed: {}", error.what());
    } catch (...) {
        logger::log_error_noexcept("app", "Node configuration action failed with an unknown exception");
    }
}

void node_manager_s::admit_action_locked(app_state_s*                        app,
                                         action_work_s&                      work,
                                         update_notices_t&                   notices,
                                         node_actions_s::clock_t::time_point now)
{
    auto& action = work.action;
    if (!node_actions_s::valid_request(action.id, action.name)) {
        action.fail(error_e::invalid_payload, "Invalid node action envelope");
        return;
    }
    const auto found = nodes_.find(action.id);
    if (found == nodes_.end() || (action.target && found->second.node->handle() != *action.target)) {
        action.fail(error_e::not_found, "Target node was removed or replaced");
        return;
    }
    auto& record                      = found->second;
    action.target                     = record.node->handle();
    auto                    candidate = record.state;
    bool                    corrected = false;
    nodes::action_context_s context(
        candidate,
        nodes_,
        [&](const json& patch) {
            auto result = record.node->set_options(candidate.options, patch);
            corrected |= result.has_corrected_values;
            return result;
        },
        work.start);
    const auto delivery = record.node->handle_action(context, action);
    if (delivery == nodes::action_dispatch_e::unhandled) {
        action.fail(error_e::unsupported_action, "Node does not support this action");
        return;
    }
    if (const auto error = action.result_error(); error && *error != error_e::no_error) {
        return;
    }
    const bool changed = candidate.options != record.state.options;
    auto*      pending = static_cast<std::deque<nodes::action_s>*>(nullptr);
    if (delivery == nodes::action_dispatch_e::frame) {
        if (!action || work.start) {
            action.fail(error_e::internal_error, "Invalid frame action dispatch");
            return;
        }
        pending = &pending_nodes_[found->first].actions;
        // Already settled entries no longer consume capacity.
        std::erase_if(*pending, [](const auto& queued) { return !queued; });
        if (pending->size() >= node_actions_s::MAX_PER_NODE) {
            action.fail(error_e::busy, "Node has too many pending frame actions");
            return;
        }
        action.sequence = next_action_sequence_++;
        action.set_deadline(now + node_actions_s::MAX_AGE,
                            app != nullptr
                                ? std::make_optional(boost::asio::any_io_executor(app->cfg_executor()->get_executor()))
                                : std::nullopt);
    } else if (delivery != nodes::action_dispatch_e::handled) {
        action.fail(error_e::internal_error, "Invalid node action dispatch");
        return;
    } else if (action && !work.start) {
        action.fail(error_e::internal_error, "Node action was not consumed");
        return;
    }
    update_notice_s* notice = nullptr;
    if (changed) {
        pending_nodes_.try_emplace(found->first);
        notice = &notices[found->first];
    }
    if (pending != nullptr) {
        pending->push_back(std::move(action));
    }
    if (changed) {
        notice->corrected |= corrected;
        notice->action_modified = true;
        record.state.options.swap(candidate.options);
    }
}

void node_manager_s::emit_option_updates_locked(const update_notices_t&             notices,
                                                const std::optional<origin_info_s>& origin)
{
    for (const auto& [id, notice] : notices) {
        for (auto& adapter : adapters_) {
            // Action-derived values must also reach their requesting editor;
            // normal option writes may suppress the originating client's echo.
            adapter->emit_update_node(
                id, nodes_.at(id).state.options, notice.corrected, notice.action_modified ? std::nullopt : origin);
        }
    }
}

nodes::set_options_result_s node_manager_s::handle_control_batch(app_state_s*                        app,
                                                                 std::span<const option_update_s>    updates,
                                                                 std::vector<action_request_s>       actions,
                                                                 const std::optional<origin_info_s>& origin,
                                                                 node_actions_s::clock_t::time_point now)
{
    auto replies = std::make_shared<action_reply_queue_s>();
    struct release_s
    {
        std::shared_ptr<action_reply_queue_s> replies;
        explicit release_s(std::shared_ptr<action_reply_queue_s> queue)
            : replies(std::move(queue))
        {
        }
        ~release_s() { replies->open(); }
        release_s(const release_s&)            = delete;
        release_s& operator=(const release_s&) = delete;
        release_s(release_s&&)                 = delete;
        release_s& operator=(release_s&&)      = delete;
    } release{replies};
    // Wrap once, before any rejection path. Destruction settles abandoned work
    // before release opens the reply queue, after all graph locks are gone.
    std::vector<action_work_s> work;
    work.reserve(actions.size());
    for (auto& request : actions) {
        work.push_back({.action = nodes::action_s(
                            std::move(request.id),
                            std::move(request.name),
                            std::move(request.payload),
                            [replies, reply = std::move(request.reply)](nodes::action_result_s result) mutable {
                                replies->push(std::move(reply), std::move(result));
                            },
                            std::move(request.target)),
                        .start = {}});
    }
    auto reject = [&](nodes::set_options_result_s result) {
        for (auto& current : work) {
            current.action.fail(result.error, "Configuration batch rejected before action processing");
        }
        return result;
    };
    nodes::set_options_result_s result{};
    {
        const std::unique_lock lock(nodes_mutex_);
        if (actions_closed_) {
            return reject({.error = error_e::cancelled, .has_corrected_values = false});
        }
        utils::unordered_string_map_t<json> candidates;
        update_notices_t                    notices;
        for (const auto& update : updates) {
            const auto found = nodes_.find(update.id);
            if (found == nodes_.end() || (update.target && found->second.node->handle() != *update.target)) {
                return reject({.error = error_e::not_found, .has_corrected_values = false});
            }
            auto [candidate, inserted] = candidates.try_emplace(update.id);
            if (inserted) {
                candidate->second = found->second.state.options;
            }
            const auto admission = found->second.node->set_options(candidate->second, update.options);
            if (admission.error != error_e::no_error) {
                return reject(admission);
            }
            notices[update.id].corrected |= admission.has_corrected_values;
            result.has_corrected_values |= admission.has_corrected_values;
        }
        // No explicit setting is committed until all updates and their pending
        // records are admitted. The graph lock covers subsequent action patches.
        for (const auto& [id, options] : candidates) {
            pending_nodes_.try_emplace(id);
        }
        for (auto& [id, options] : candidates) {
            nodes_.at(id).state.options.swap(options);
        }
        for (auto& current : work) {
            try {
                admit_action_locked(app, current, notices, now);
            } catch (const std::exception& error) {
                logger::log_error_noexcept("app", "Node action admission failed: {}", error.what());
                current.action.fail(error_e::internal_error, "Node action admission failed");
            } catch (...) {
                current.action.fail(error_e::internal_error, "Node action admission failed");
            }
        }
        emit_option_updates_locked(notices, origin);
    }
    for (auto& current : work) {
        current.run(app);
    }
    return result;
}

error_e node_manager_s::handle_node_action(app_state_s*            app,
                                           std::string_view        id,
                                           std::string_view        name,
                                           const json&             payload,
                                           node_actions_s::reply_t reply)
{
    if (!reply || !node_actions_s::valid_request(id, name)) {
        return error_e::invalid_payload;
    }
    std::vector<action_request_s> actions;
    actions.push_back(
        {.id = std::string(id), .name = std::string(name), .payload = payload, .reply = std::move(reply)});
    (void)handle_control_batch(app, {}, std::move(actions));
    // Once wrapped, all errors are reported through the action, never twice.
    return error_e::no_error;
}

node_actions_s::batch_t node_manager_s::take_frame_updates()
{
    node_actions_s::batch_t actions;
    const std::unique_lock  lock(nodes_mutex_);
    actions.groups_.reserve(pending_nodes_.size());
    for (const auto& id : removed_nodes_) {
        nodes_copy_.erase(id);
    }
    for (const auto& [id, pending] : pending_nodes_) {
        if (const auto it = nodes_.find(id); it != nodes_.end()) {
            nodes_copy_.insert_or_assign(it->first, it->second);
        }
    }
    // Only transfer completion ownership once the complete snapshot is ready.
    for (auto& [id, pending] : pending_nodes_) {
        if (!pending.actions.empty()) {
            actions.groups_.push_back(std::move(pending));
        }
    }
    pending_nodes_.clear();
    removed_nodes_.clear();
    next_action_sequence_ = 0;
    return actions;
}

node_manager_s::~node_manager_s()
{
    try {
        close_actions();
    } catch (...) {
        logger::log_error_noexcept("app", "Failed to close node actions during shutdown");
        std::terminate();
    }
}

void node_manager_s::close_actions()
{
    utils::unordered_string_map_t<node_actions_s::pending_s> pending;
    {
        const std::unique_lock lock(nodes_mutex_);
        actions_closed_ = true;
        pending.swap(pending_nodes_);
        for (const auto& [id, record] : nodes_) {
            record.node->handle().retire();
        }
    }
    for (auto& [id, node] : pending) {
        for (auto& action : node.actions) {
            action.fail(error_e::cancelled, "Node actions are shutting down");
        }
    }
}

nlohmann::json node_manager_s::get_node_status(std::string_view id) const
{
    if (status_registry_ == nullptr) {
        return nlohmann::json::object();
    }
    return status_registry_->get(id);
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

void node_manager_s::tick_one_frame(app_state_s* app, frame_scheduler_s& scheduler)
{
    // One graph-lock boundary transfers settings and transient actions together.
    auto actions = take_frame_updates();
    {
        const auto settings = nodes_copy_.find(nodes::system::SETTINGS_NODE_ID);
        if (settings == nodes_copy_.end()) {
            throw std::logic_error("Application settings node is missing from the render snapshot");
        }
        const auto& settings_state           = settings->second.state;
        const auto  frame_rate               = settings_state.options.at("frame_rate").get<frame_rate_s>();
        const auto  default_framebuffer_size = settings_state.options.at("default_framebuffer_size").get<gpu::vec2_t>();
        const auto  decklink_output_buffer_frames =
            settings_state.options.at("decklink_output_buffer_frames").get<int>();
        const auto ndi_output_buffer_frames     = settings_state.options.at("ndi_output_buffer_frames").get<int>();
        const auto screen_output_buffer_frames  = settings_state.options.at("screen_output_buffer_frames").get<int>();
        auto       frame_settings               = app_state_s::frame_settings_s{};
        frame_settings.frame_rate               = frame_rate;
        frame_settings.framebuffer.default_size = {
            static_cast<int>(default_framebuffer_size.x),
            static_cast<int>(default_framebuffer_size.y),
        };
        frame_settings.decklink_output.buffer_frames = decklink_output_buffer_frames;
        frame_settings.ndi_output.buffer_frames      = ndi_output_buffer_frames;
        frame_settings.screen_output.buffer_frames   = screen_output_buffer_frames;
        app->begin_frame(frame_settings, scheduler.begin_frame(frame_rate));

        {
            const auto& frame_context = app->frame_context();
            app->status_registry()->write(settings_status_handle_,
                                          status::application_frame_status_s{
                                              .frame_rate            = app->frame_settings().frame_rate,
                                              .frame_duration_flicks = frame_context.frame_duration.count(),
                                              .epoch                 = frame_context.epoch,
                                          },
                                          reported_status_epoch_.observe(frame_context.epoch)
                                              ? status_delivery_e::immediate
                                              : status_delivery_e::rate_limited);
        }

        actions.dispatch(app, nodes_copy_);
        const auto prepare_start   = utils::flicks_now();
        const auto demanding_nodes = nodes::prepare_all_nodes(app, nodes_copy_);
        const auto prepare_end     = utils::flicks_now();

        app->frame_info.submitted_nodes.clear();
        app->frame_info.submitted_nodes.reserve(nodes_copy_.size());
        nodes::submit_demanding_nodes(app, nodes_copy_, demanding_nodes);
        const auto submit_end = utils::flicks_now();

        app->frame_info.executed_nodes.clear();
        app->frame_info.executed_nodes.reserve(nodes_copy_.size());
        try {
            const app_state_s::frame_scope_s frame(*app);
            nodes::execute_demanding_nodes(app, nodes_copy_, demanding_nodes);
            app->commit_gpu_frame();
        } catch (const gpu::recording_unavailable_s&) {
            ++gpu_recording_drops_;
        } catch (...) {
            nodes::complete_all_nodes(app, nodes_copy_);
            throw;
        }

        const auto execute_end = utils::flicks_now();

        const auto finish_end = execute_end;

        nodes::complete_all_nodes(app, nodes_copy_);
        const auto complete_end = utils::flicks_now();

        {
            const auto to_microseconds = [](utils::flicks duration) {
                return std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
            };
            app->status_registry()->write(settings_status_handle_,
                                          status::application_lifecycle_status_s{
                                              .prepare_duration_us    = to_microseconds(prepare_end - prepare_start),
                                              .submit_duration_us     = to_microseconds(submit_end - prepare_end),
                                              .execute_duration_us    = to_microseconds(execute_end - submit_end),
                                              .gpu_finish_duration_us = to_microseconds(finish_end - execute_end),
                                              .complete_duration_us   = to_microseconds(complete_end - finish_end),
                                              .demanding_node_count   = demanding_nodes.size(),
                                              .submitted_node_count   = app->frame_info.submitted_nodes.size(),
                                              .executed_node_count    = app->frame_info.executed_nodes.size(),
                                              .gpu_recording_drops    = gpu_recording_drops_,
                                          });
        }
    }

    // Transfer owned typed status only after every node has completed the frame.
    app->status_registry()->publish();
}

void node_manager_s::clear_nodes(app_state_s* app)
{
    close_actions();
    app->abort_gpu();

    nodes_copy_.clear();
    nodes_.clear();
    connections_.clear();
    removed_nodes_.clear();
}

std::pair<std::shared_ptr<nodes::node_i>, error_e> node_manager_s::create_node(std::string_view type)
{
    auto it = node_definitions_.find(type);
    if (it == node_definitions_.end()) {
        return {nullptr, error_e::invalid_type};
    }

    auto n = it->second.constructor();
    if (n->type() != type) {
        return {nullptr, error_e::internal_error};
    }

    return {n, error_e::no_error};
}

} // namespace miximus::core
