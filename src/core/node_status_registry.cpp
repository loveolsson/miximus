#include "node_status_registry.hpp"

#include "logger/logger.hpp"
#include "utils/string_map.hpp"

#include <boost/asio/post.hpp>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <mutex>
#include <unordered_map>

namespace miximus::core {

struct node_status_registry_s::state_s
{
    struct entry_s
    {
        uint64_t                 sequence;
        std::unique_ptr<value_i> value;
    };
    using groups_t = std::unordered_map<std::type_index, entry_s>;
    using batch_t  = std::unordered_map<std::shared_ptr<node_status_handle_s::identity_s>, groups_t>;
    struct snapshot_s
    {
        std::shared_ptr<node_status_handle_s::identity_s> identity;
        nlohmann::json                                    status = nlohmann::json::object();
    };

    explicit state_s(boost::asio::io_context& context)
        : executor(context)
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
    utils::unordered_string_map_t<snapshot_s> snapshots;
    callback_t                                callback;
};

node_status_registry_s::node_status_registry_s(boost::asio::io_context& executor)
    : state_(std::make_shared<state_s>(executor))
{
}
node_status_registry_s::~node_status_registry_s() { stop(); }

void node_status_registry_s::set_callback(callback_t callback) { state_->callback = std::move(callback); }
void node_status_registry_s::stop() noexcept { state_->stopped.store(true); }

void node_status_registry_s::enqueue(const node_status_handle_s& node,
                                     std::type_index             type,
                                     std::unique_ptr<value_i>    value)
{
    if (!state_->stopped.load()) {
        state_->frame[node.identity_].insert_or_assign(type, state_s::entry_s{++state_->sequence, std::move(value)});
    }
}

void node_status_registry_s::schedule(const std::shared_ptr<state_s>& state)
{
    try {
        boost::asio::post(state->executor, [state] { drain(state); });
    } catch (...) {
        const std::scoped_lock lock(state->mutex);
        state->scheduled = false;
        throw;
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
        if (state.pending.empty()) {
            state.pending.swap(state.frame);
        } else {
            for (auto it = state.pending.begin(); it != state.pending.end();) {
                if (!it->first->active.load()) {
                    auto old = it++;
                    retired.insert(state.pending.extract(old));
                } else {
                    ++it;
                }
            }
            for (auto& [node, groups] : state.frame) {
                if (!node->active.load()) {
                    continue;
                }
                auto& pending = state.pending[node];
                for (auto& [type, entry] : groups) {
                    if (auto it = pending.find(type); it != pending.end()) {
                        std::swap(it->second, entry);
                    } else {
                        pending.emplace(type, std::move(entry));
                    }
                }
            }
        }
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

    std::vector<status_update_s> updates;
    for (auto& [node, groups] : batch) {
        if (!node->active.load()) {
            continue;
        }
        auto& snapshot = state->snapshots[node->id];
        if (snapshot.identity != node) {
            snapshot = state_s::snapshot_s{.identity = node};
        }
        // Different contracts may share a wire field. Preserve last-write order
        // without teaching the mailbox anything about those contracts.
        std::vector<state_s::entry_s*> ordered;
        ordered.reserve(groups.size());
        for (auto& [type, entry] : groups) {
            ordered.push_back(&entry);
        }
        std::ranges::sort(ordered, {}, &state_s::entry_s::sequence);
        auto delta = nlohmann::json::object();
        for (const auto* entry : ordered) {
            try {
                const auto json = entry->value->serialize();
                for (const auto& [name, value] : json.items()) {
                    delta[name] = value;
                }
            } catch (const std::exception& error) {
                logger::log_error_noexcept("app", "Failed to serialize status for {}: {}", node->id, error.what());
            }
        }
        for (auto it = delta.begin(); it != delta.end();) {
            const auto current = snapshot.status.find(it.key());
            if (current != snapshot.status.end() && *current == it.value()) {
                it = delta.erase(it);
            } else {
                snapshot.status[it.key()] = it.value();
                ++it;
            }
        }
        if (!delta.empty()) {
            updates.push_back({node->id, std::move(delta)});
        }
    }
    if (!updates.empty() && state->callback && !state->stopped.load()) {
        try {
            state->callback(updates);
        } catch (const std::exception& error) {
            logger::log_error_noexcept("app", "Failed to broadcast node status: {}", error.what());
        }
    }
    bool post;
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

void node_status_registry_s::remove_node(const node_status_handle_s& node)
{
    node.retire();
    if (!node.identity_) {
        return;
    }
    const auto it = state_->snapshots.find(node.identity_->id);
    if (it != state_->snapshots.end() && it->second.identity == node.identity_) {
        state_->snapshots.erase(it);
    }
}

nlohmann::json node_status_registry_s::get(std::string_view node_id) const
{
    const auto it = state_->snapshots.find(node_id);
    return it != state_->snapshots.end() && it->second.identity->active.load() ? it->second.status
                                                                               : nlohmann::json::object();
}

nlohmann::json node_status_registry_s::get_all() const
{
    auto result = nlohmann::json::object();
    for (const auto& [id, snapshot] : state_->snapshots) {
        if (snapshot.identity->active.load()) {
            result[id] = snapshot.status;
        }
    }
    return result;
}

} // namespace miximus::core
