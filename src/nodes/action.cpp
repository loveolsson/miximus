#include "action.hpp"

#include "logger/logger.hpp"
#include "nodes/node_map.hpp"

#include <boost/asio/steady_timer.hpp>

#include <exception>
#include <mutex>
#include <utility>

namespace miximus::nodes {
struct action_s::response_s
{
    mutable std::mutex                         mutex;
    reply_t                                    reply;
    std::optional<error_e>                     result;
    std::optional<clock_t::time_point>         deadline;
    std::unique_ptr<boost::asio::steady_timer> timer;

    explicit response_s(reply_t callback)
        : reply(std::move(callback))
    {
    }
    static void deliver(const reply_t& reply, action_result_s result) noexcept
    {
        if (!reply) {
            return;
        }
        try {
            reply(std::move(result));
        } catch (const std::exception& error) {
            logger::log_error_noexcept("app", "Node action reply failed: {}", error.what());
        } catch (...) {
            logger::log_error_noexcept("app", "Node action reply failed with an unknown exception");
        }
    }
    void cancel_timer() noexcept
    {
        if (!timer) {
            return;
        }
        try {
            timer->cancel();
        } catch (...) {
            logger::log_error_noexcept("app", "Node action timer cancellation failed");
        }
        timer.reset();
    }
    reply_t settle_locked(error_e error)
    {
        if (result) {
            return {};
        }
        result = error;
        deadline.reset();
        cancel_timer();
        return std::exchange(reply, {});
    }
    void complete(action_result_s value) noexcept
    {
        reply_t callback;
        {
            const std::scoped_lock lock(mutex);
            callback = settle_locked(value.error);
        }
        deliver(callback, std::move(value));
    }
    bool check_deadline(clock_t::time_point now, bool consume) noexcept
    {
        reply_t callback;
        bool    pending{false};
        {
            const std::scoped_lock lock(mutex);
            if (!result && deadline && now >= *deadline) {
                callback = settle_locked(error_e::expired);
            }
            if (consume) {
                deadline.reset();
                cancel_timer();
            }
            pending = !result;
        }
        deliver(callback, {.error = error_e::expired});
        return pending;
    }
};

action_s::action_s(std::string                        node_id,
                   std::string                        action_name,
                   nlohmann::json                     action_payload,
                   reply_t                            reply,
                   std::optional<core::node_handle_s> node_target)
    : response_(std::make_shared<response_s>(std::move(reply)))
    , owns_(true)
    , id(std::move(node_id))
    , name(std::move(action_name))
    , payload(std::move(action_payload))
    , target(std::move(node_target))
{
}
action_s::~action_s() { fail(error_e::internal_error, "Node action was abandoned without a result"); }
action_s::action_s(action_s&& other) noexcept
    // The source keeps a read-only outcome observer; owns_ alone transfers response authority.
    // NOLINTNEXTLINE(performance-move-constructor-init)
    : response_(other.response_)
    , owns_(std::exchange(other.owns_, false))
    , id(std::move(other.id))
    , name(std::move(other.name))
    , payload(std::move(other.payload))
    , target(std::move(other.target))
    , sequence(other.sequence)
{
}
action_s& action_s::operator=(action_s&& other) noexcept
{
    if (this != &other) {
        fail(error_e::internal_error, "Node action was abandoned without a result");
        response_ = other.response_;
        owns_     = std::exchange(other.owns_, false);
        id        = std::move(other.id);
        name      = std::move(other.name);
        payload   = std::move(other.payload);
        target    = std::move(other.target);
        sequence  = other.sequence;
    }
    return *this;
}
void action_s::complete(action_result_s result) noexcept
{
    if (owns_) {
        response_->complete(std::move(result));
    }
}
void action_s::fail(error_e error, std::string_view message) noexcept
{
    if (!*this) {
        return;
    }
    try {
        complete({.error = error, .message = std::string(message)});
    } catch (...) {
        complete({.error = error});
    }
}
action_s::             operator bool() const noexcept { return owns_ && !result_error(); }
std::optional<error_e> action_s::result_error() const noexcept
{
    if (!response_) {
        return {};
    }
    const std::scoped_lock lock(response_->mutex);
    return response_->result;
}
void action_s::set_deadline(clock_t::time_point deadline, std::optional<boost::asio::any_io_executor> executor)
{
    if (!owns_) {
        return;
    }
    const std::scoped_lock lock(response_->mutex);
    if (response_->result) {
        return;
    }
    response_->deadline = deadline;
    response_->cancel_timer();
    if (executor) {
        response_->timer = std::make_unique<boost::asio::steady_timer>(*executor, deadline);
        response_->timer->async_wait([weak = std::weak_ptr(response_)](const boost::system::error_code& error) {
            if (!error) {
                if (auto response = weak.lock()) {
                    response->check_deadline(clock_t::now(), false);
                }
            }
        });
    }
}
bool action_s::expire(clock_t::time_point now) noexcept
{
    if (!owns_) {
        return false;
    }
    response_->check_deadline(now, false);
    return result_error() == error_e::expired;
}
bool action_s::consume(clock_t::time_point now) noexcept { return owns_ && response_->check_deadline(now, true); }
const node_state_s* action_context_s::find_state(std::string_view id) const
{
    const auto found = graph_.find(id);
    return found == graph_.end() ? nullptr : &found->second.state;
}
} // namespace miximus::nodes
