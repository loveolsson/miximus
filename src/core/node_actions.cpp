#include "node_actions.hpp"

#include "logger/logger.hpp"
#include "nodes/node.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace miximus::core {
namespace {

void reply(const node_actions_s::reply_t& callback, nodes::action_result_s result)
{
    if (!callback) {
        return;
    }
    // Transport teardown or a faulty responder must not interrupt graph work
    // or prevent the remaining requests from receiving their replies.
    try {
        callback(std::move(result));
    } catch (const std::exception& error) {
        if (const auto log = getlog("app")) {
            log->error("Node action reply failed: {}", error.what());
        }
    } catch (...) {
        if (const auto log = getlog("app")) {
            log->error("Node action reply failed with an unknown exception");
        }
    }
}

nodes::action_result_s execute(const node_actions_s::request_s&    request,
                               app_state_s*                        app,
                               const nodes::node_map_t&            snapshot,
                               node_actions_s::clock_t::time_point now)
{
    if (now >= request.deadline) {
        return {.error = error_e::expired, .message = "Node action expired before dispatch"};
    }
    const auto found  = snapshot.find(request.id);
    const auto target = request.target.lock();
    if (!target || found == snapshot.end() || found->second.node != target) {
        return {.error = error_e::not_found, .message = "Target node was removed or replaced"};
    }
    try {
        auto result = target->handle_action(app, found->second.state, request.name, request.payload);
        if (result.message.size() > 1024 || !node_action_limits::valid_payload(result.message) ||
            !node_action_limits::valid_payload(result.data)) {
            return {.error = error_e::internal_error, .message = "Node action result exceeds its size limit"};
        }
        return result;
    } catch (const std::exception& error) {
        if (const auto log = getlog("app")) {
            log->error("Node action {} on {} failed: {}", request.name, request.id, error.what());
        }
        return {.error = error_e::internal_error, .message = "Node action handler failed"};
    } catch (...) {
        return {.error = error_e::internal_error, .message = "Node action handler failed"};
    }
}
} // namespace

node_actions_s::batch_t::~batch_t()
{
    for (const auto& request : requests_) {
        reply(request.reply, {.error = error_e::cancelled, .message = "Node action frame was abandoned"});
    }
}

node_actions_s::batch_t& node_actions_s::batch_t::operator=(batch_t&& other) noexcept
{
    batch_t previous;
    previous.requests_.swap(requests_);
    requests_.swap(other.requests_);
    return *this;
}

error_e node_actions_s::enqueue(std::string_view             id,
                                std::weak_ptr<nodes::node_i> target,
                                std::string_view             name,
                                const nlohmann::json&        payload,
                                reply_t                      callback,
                                clock_t::time_point          now)
{
    if (id.empty() || id.size() > node_action_limits::MAX_ID_BYTES || name.empty() ||
        name.size() > node_action_limits::MAX_NAME_BYTES || !callback || !node_action_limits::valid_payload(payload)) {
        return error_e::invalid_payload;
    }
    if (target.expired()) {
        return error_e::not_found;
    }
    // Prepare owned values outside the inbox lock; the render thread only waits
    // for admission bookkeeping and moving a bounded batch, never a JSON copy.
    request_s              request{.id       = std::string(id),
                                   .target   = std::move(target),
                                   .name     = std::string(name),
                                   .payload  = payload,
                                   .reply    = std::move(callback),
                                   .deadline = now + MAX_AGE};
    const std::scoped_lock lock(mutex_);
    if (closed_) {
        return error_e::cancelled;
    }
    if (pending_.size() >= MAX_PENDING || std::ranges::count_if(pending_, [id](const auto& request) {
                                              return request.id == id;
                                          }) >= static_cast<std::ptrdiff_t>(MAX_PER_NODE)) {
        return error_e::busy;
    }
    pending_.push_back(std::move(request));
    return error_e::no_error;
}

node_actions_s::batch_t node_actions_s::take_batch()
{
    const std::scoped_lock lock(mutex_);
    batch_t                batch;
    const auto             count = std::min(MAX_PER_FRAME, pending_.size());
    batch.requests_.reserve(count);
    for (size_t index = 0; index < count; ++index) {
        batch.requests_.push_back(std::move(pending_.front()));
        pending_.pop_front();
    }
    return batch;
}

void node_actions_s::dispatch(batch_t                  batch,
                              app_state_s*             app,
                              const nodes::node_map_t& snapshot,
                              clock_t::time_point      now)
{
    for (auto& request : batch.requests_) {
        auto result   = execute(request, app, snapshot, now);
        auto callback = std::exchange(request.reply, {});
        reply(callback, std::move(result));
    }
}

void node_actions_s::close()
{
    std::deque<request_s> cancelled;
    {
        const std::scoped_lock lock(mutex_);
        closed_ = true;
        cancelled.swap(pending_);
    }
    for (const auto& request : cancelled) {
        reply(request.reply, {.error = error_e::cancelled, .message = "Node actions are shutting down"});
    }
}

} // namespace miximus::core
