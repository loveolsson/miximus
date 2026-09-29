#include "node_actions.hpp"

#include "logger/logger.hpp"
#include "nodes/node.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace miximus::core {
bool node_actions_s::valid_request(std::string_view id, std::string_view name) { return !id.empty() && !name.empty(); }
node_actions_s::batch_t::~batch_t() { cancel(); }
node_actions_s::batch_t::batch_t(batch_t&& other) noexcept { groups_.swap(other.groups_); }
node_actions_s::batch_t& node_actions_s::batch_t::operator=(batch_t&& other) noexcept
{
    batch_t previous;
    previous.groups_.swap(groups_);
    groups_.swap(other.groups_);
    return *this;
}
size_t node_actions_s::batch_t::size() const
{
    size_t count{};
    for (const auto& group : groups_) {
        count += group.actions.size();
    }
    return count;
}
void node_actions_s::batch_t::cancel(error_e error, std::string_view message)
{
    for (auto& group : groups_) {
        for (auto& request : group.actions) {
            request.fail(error, message);
        }
    }
    groups_.clear();
}
void node_actions_s::batch_t::dispatch(app_state_s* app, const nodes::node_map_t& snapshot, clock_t::time_point now)
{
    std::vector<request_s*> ordered;
    ordered.reserve(size());
    for (auto& group : groups_) {
        for (auto& request : group.actions) {
            ordered.push_back(&request);
        }
    }
    std::ranges::sort(ordered, {}, [](const request_s* request) { return request->sequence; });
    for (auto* entry : ordered) {
        auto& request = *entry;
        if (!request.consume(now)) {
            continue;
        }
        const auto found = snapshot.find(request.id);
        if (found == snapshot.end() || !request.target || found->second.node->handle() != *request.target) {
            request.fail(error_e::not_found, "Target node was removed or replaced");
            continue;
        }
        auto action = std::move(request);
        try {
            if (found->second.node->handle_frame_action(app, found->second.state, action) !=
                nodes::action_dispatch_e::handled) {
                action.fail(error_e::internal_error, "Node did not handle frame action");
            }
        } catch (const std::exception& error) {
            logger::log_error_noexcept("app", "Node frame action failed: {}", error.what());
            action.fail(error_e::internal_error, "Node frame action failed");
        } catch (...) {
            action.fail(error_e::internal_error, "Node frame action failed");
        }
        // Unsettled ownership remaining here fails on destruction.
    }
    groups_.clear();
}
} // namespace miximus::core
