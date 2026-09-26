#include "node_status_registry.hpp"

#include "logger/logger.hpp"
#include "utils/string_map.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace miximus::core {

struct node_status_registry_s::state_s
{
    struct entry_s
    {
        uint64_t                 sequence;
        status_delivery_e        delivery;
        std::unique_ptr<value_i> value;
    };
    using groups_t = std::unordered_map<std::type_index, entry_s>;
    using batch_t  = std::unordered_map<std::shared_ptr<node_status_handle_s::identity_s>, groups_t>;

    // Caller holds mutex; displaced payloads stay in frame/retired until it is released.
    void merge_frame(batch_t& retired);
    struct snapshot_s
    {
        std::shared_ptr<node_status_handle_s::identity_s> identity;
        groups_t                                          pending;
        nlohmann::json                                    status    = nlohmann::json::object();
        nlohmann::json                                    broadcast = nlohmann::json::object();
        std::unordered_set<std::string>                   dirty_fields;
        std::chrono::steady_clock::time_point             next_report;
        bool                                              immediate{};

        bool dirty() const { return !pending.empty() || !dirty_fields.empty(); }
        void materialize();
    };

    explicit state_s(boost::asio::io_context& context, std::chrono::steady_clock::duration interval)
        : executor(context)
        , report_interval(interval)
        , timer(context)
    {
    }
    boost::asio::io_context& executor;
    std::atomic<bool>        stopped{false};
    // Producer-owned, never touched by the executor.
    batch_t  frame;
    uint64_t sequence{};
    // Only typed mailbox transfers use this lock. Never serialize, read snapshots,
    // broadcast, or destroy a displaced payload while holding it.
    std::mutex mutex;
    batch_t    pending;
    bool       scheduled{};
    // Configuration executor owned.
    utils::unordered_string_map_t<snapshot_s>            snapshots;
    callback_t                                           callback;
    const std::chrono::steady_clock::duration            report_interval;
    boost::asio::steady_timer                            timer;
    std::optional<std::chrono::steady_clock::time_point> timer_deadline;
};

node_status_registry_s::node_status_registry_s(boost::asio::io_context&            executor,
                                               std::chrono::steady_clock::duration report_interval)
    : state_(std::make_shared<state_s>(executor, report_interval))
{
    if (report_interval <= std::chrono::steady_clock::duration::zero()) {
        throw std::invalid_argument("Status report interval must be positive");
    }
}
node_status_registry_s::~node_status_registry_s() { stop(); }

void node_status_registry_s::set_callback(callback_t callback) { state_->callback = std::move(callback); }
void node_status_registry_s::stop() noexcept
{
    if (state_->stopped.exchange(true)) {
        return;
    }
    // Timer operations belong to the configuration executor, including teardown.
    try {
        boost::asio::post(state_->executor, [state = state_] {
            state->timer.cancel();
            state->timer_deadline.reset();
        });
    } catch (const std::exception& error) {
        logger::log_error_noexcept("app", "Failed to cancel status timer: {}", error.what());
    }
}

void node_status_registry_s::enqueue(const node_status_handle_s& node,
                                     std::type_index             type,
                                     std::unique_ptr<value_i>    value,
                                     status_delivery_e           delivery)
{
    if (!state_->stopped.load()) {
        auto& groups = state_->frame[node.identity_];
        if (const auto previous = groups.find(type); previous != groups.end()) {
            if (previous->second.delivery == status_delivery_e::immediate) {
                delivery = status_delivery_e::immediate;
            }
        }
        groups.insert_or_assign(
            type, state_s::entry_s{.sequence = ++state_->sequence, .delivery = delivery, .value = std::move(value)});
    }
}

// asio::post always queues this handler; it cannot recursively call drain on this stack.
// NOLINTNEXTLINE(misc-no-recursion)
void node_status_registry_s::schedule(const std::shared_ptr<state_s>& state)
{
    try {
        // NOLINTNEXTLINE(misc-no-recursion) -- queued continuation, never an inline call.
        boost::asio::post(state->executor, [state] { drain(state); });
    } catch (...) {
        const std::scoped_lock lock(state->mutex);
        state->scheduled = false;
        throw;
    }
}

void node_status_registry_s::state_s::merge_frame(batch_t& retired)
{
    if (pending.empty()) {
        pending.swap(frame);
        return;
    }
    for (auto it = pending.begin(); it != pending.end();) {
        if (!it->first->active.load()) {
            auto old = it++;
            retired.insert(pending.extract(old));
        } else {
            ++it;
        }
    }
    for (auto& [node, groups] : frame) {
        if (!node->active.load()) {
            continue;
        }
        auto& pending_groups = pending[node];
        for (auto& [type, entry] : groups) {
            if (auto it = pending_groups.find(type); it != pending_groups.end()) {
                if (it->second.delivery == status_delivery_e::immediate) {
                    entry.delivery = status_delivery_e::immediate;
                }
                std::swap(it->second, entry);
            } else {
                pending_groups.emplace(type, std::move(entry));
            }
        }
    }
}

void node_status_registry_s::publish()
{
    auto& state = *state_;
    if (state.stopped.load()) {
        return;
    }
    bool post = false;
    // Swapped-out groups and replaced values are destroyed after unlocking.
    state_s::batch_t retired;
    {
        const std::scoped_lock lock(state.mutex);
        state.merge_frame(retired);
        if (!state.pending.empty() && !state.scheduled) {
            state.scheduled = true;
            post            = true;
        }
    }
    state.frame.clear();
    if (post) {
        schedule(state_);
    }
}

// NOLINTNEXTLINE(misc-no-recursion) -- schedule uses asio::post, so each drain starts on a fresh stack.
void node_status_registry_s::drain(const std::shared_ptr<state_s>& state)
{
    state_s::batch_t batch;
    {
        const std::scoped_lock lock(state->mutex);
        batch.swap(state->pending);
    }
    if (state->stopped.load()) {
        return;
    }

    for (auto& [node, groups] : batch) {
        if (!node->active.load()) {
            continue;
        }
        auto& snapshot = state->snapshots[node->id];
        if (snapshot.identity != node) {
            snapshot          = state_s::snapshot_s{};
            snapshot.identity = node;
        }
        for (auto& [type, entry] : groups) {
            snapshot.immediate = snapshot.immediate || entry.delivery == status_delivery_e::immediate;
            snapshot.pending.insert_or_assign(type, std::move(entry));
        }
    }
    emit_due(state);
    bool post = false;
    {
        const std::scoped_lock lock(state->mutex);
        post             = !state->pending.empty() && !state->stopped.load();
        state->scheduled = post;
    }
    // Yield between batches so HTTP and graph commands cannot be starved by telemetry.
    if (post) {
        schedule(state);
    }
}

void node_status_registry_s::state_s::snapshot_s::materialize()
{
    std::vector<entry_s*> ordered;
    ordered.reserve(pending.size());
    for (auto& [type, entry] : pending) {
        ordered.push_back(&entry);
    }
    // Last-write semantics remain valid even if contract types share fields.
    std::ranges::sort(ordered, {}, &entry_s::sequence);
    for (const auto* entry : ordered) {
        try {
            auto json = entry->value->serialize();
            for (auto& [name, value] : json.items()) {
                dirty_fields.insert(name);
                status[name] = std::move(value);
            }
        } catch (const std::exception& error) {
            logger::log_error_noexcept("app", "Failed to serialize status for {}: {}", identity->id, error.what());
        }
    }
    pending.clear();
}

void node_status_registry_s::emit_due(const std::shared_ptr<state_s>& state)
{
    if (state->stopped.load()) {
        return;
    }
    const auto                   now = std::chrono::steady_clock::now();
    std::vector<status_update_s> updates;
    for (auto& [id, snapshot] : state->snapshots) {
        if (!snapshot.identity->active.load() || !snapshot.dirty() ||
            (!snapshot.immediate && now < snapshot.next_report)) {
            continue;
        }
        snapshot.materialize();
        auto delta = nlohmann::json::object();
        for (const auto& name : snapshot.dirty_fields) {
            const auto& value    = snapshot.status.at(name);
            const auto  previous = snapshot.broadcast.find(name);
            if (previous == snapshot.broadcast.end() || *previous != value) {
                delta[name]              = value;
                snapshot.broadcast[name] = value;
            }
        }
        snapshot.dirty_fields.clear();
        snapshot.immediate = false;
        // Also bound conversion when all values are unchanged. An unchanged
        // immediate request does not postpone an already scheduled normal report.
        if (now >= snapshot.next_report) {
            snapshot.next_report = now + state->report_interval;
        }
        if (!delta.empty()) {
            updates.push_back({.node_id = id, .status = std::move(delta)});
        }
    }
    if (!updates.empty() && state->callback && !state->stopped.load()) {
        try {
            state->callback(updates);
        } catch (const std::exception& error) {
            logger::log_error_noexcept("app", "Failed to broadcast node status: {}", error.what());
        }
    }
    // Measure the next interval from actual delivery, not from the producer's
    // clock or an overdue timer. A busy executor must not cause catch-up bursts.
    const auto next = std::chrono::steady_clock::now() + state->report_interval;
    for (const auto& update : updates) {
        if (const auto it = state->snapshots.find(update.node_id); it != state->snapshots.end()) {
            it->second.next_report = next;
        }
    }
    arm_timer(state);
}

void node_status_registry_s::arm_timer(const std::shared_ptr<state_s>& state)
{
    if (state->stopped.load()) {
        return;
    }
    std::optional<std::chrono::steady_clock::time_point> deadline;
    for (const auto& [id, snapshot] : state->snapshots) {
        if (snapshot.identity->active.load() && snapshot.dirty() && (!deadline || snapshot.next_report < *deadline)) {
            deadline = snapshot.next_report;
        }
    }
    if (deadline == state->timer_deadline) {
        return;
    }
    state->timer_deadline = deadline;
    if (!deadline) {
        state->timer.cancel();
        return;
    }
    state->timer.expires_at(*deadline);
    state->timer.async_wait([state, deadline](const boost::system::error_code& error) {
        if (error || state->stopped.load() || state->timer_deadline != deadline) {
            return;
        }
        state->timer_deadline.reset();
        emit_due(state);
    });
}

void node_status_registry_s::remove_node(const node_status_handle_s& node)
{
    node.retire();
    if (!node.identity_) {
        return;
    }
    const auto it = state_->snapshots.find(node.identity_->id);
    if (it != state_->snapshots.end() && it->second.identity == node.identity_) {
        state_->snapshots.erase(it);
        arm_timer(state_);
    }
}

nlohmann::json node_status_registry_s::get(std::string_view node_id) const
{
    const auto it = state_->snapshots.find(node_id);
    if (it == state_->snapshots.end() || !it->second.identity->active.load()) {
        return nlohmann::json::object();
    }
    it->second.materialize();
    return it->second.status;
}

nlohmann::json node_status_registry_s::get_all() const
{
    auto result = nlohmann::json::object();
    for (auto& [id, snapshot] : state_->snapshots) {
        if (snapshot.identity->active.load()) {
            snapshot.materialize();
            result[id] = snapshot.status;
        }
    }
    return result;
}

} // namespace miximus::core
