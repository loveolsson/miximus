#pragma once

#include "core/app_state_fwd.hpp"
#include "core/node_handle.hpp"
#include "nodes/node_map_fwd.hpp"
#include "nodes/option_result.hpp"
#include "types/error.hpp"

#include <boost/asio/any_io_executor.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace miximus::nodes {
struct action_result_s
{
    error_e        error{error_e::no_error};
    std::string    message{};
    nlohmann::json data = nullptr;
};

enum class action_dispatch_e
{
    unhandled,
    handled,
    frame
};

// One request, one owning handler. Moves transfer responsibility for replying.
// Settings, frame state and transaction policy never belong to this object.
class action_s
{
    struct response_s;
    std::shared_ptr<response_s> response_;
    bool                        owns_{};

  public:
    using clock_t = std::chrono::steady_clock;
    using reply_t = std::function<void(action_result_s)>;
    std::string                        id;
    std::string                        name;
    nlohmann::json                     payload;
    std::optional<core::node_handle_s> target;
    size_t                             sequence{};

    action_s(std::string                        id,
             std::string                        name,
             nlohmann::json                     payload,
             reply_t                            reply,
             std::optional<core::node_handle_s> target = {});
    ~action_s();
    action_s(const action_s&)            = delete;
    action_s& operator=(const action_s&) = delete;
    action_s(action_s&& other) noexcept;
    action_s& operator=(action_s&& other) noexcept;

    void     complete(action_result_s result = {}) noexcept;
    void     fail(error_e error, std::string_view message = {}) noexcept;
    explicit operator bool() const noexcept;
    // An observer remains usable after moving, so the caller can inspect the
    // outcome without retaining authority to reply through the moved-from object.
    std::optional<error_e> result_error() const noexcept;

    // Timer callbacks share only response state, never the action or node.
    // Without an executor, expiry is checked explicitly by the owning queue.
    void set_deadline(clock_t::time_point deadline, std::optional<boost::asio::any_io_executor> executor = {});
    bool expire(clock_t::time_point now = clock_t::now()) noexcept;
    bool consume(clock_t::time_point now = clock_t::now()) noexcept;

    template <typename T>
    std::optional<T> get_typed_payload() const
    {
        try {
            return payload.get<T>();
        } catch (const nlohmann::json::exception&) {
            return std::nullopt;
        }
    }
};

// Borrowed config-thread capability. Only handle_action receives this interface.
// Settings and scheduling are owned by its caller; no reference may escape.
class action_context_s
{
  public:
    using start_t  = std::function<void(core::app_state_s*, action_s)>;
    using update_t = std::function<set_options_result_s(const nlohmann::json&)>;

  private:
    const node_state_s& state_;
    const node_map_t&   graph_;
    update_t            update_;
    start_t&            start_;

  public:
    action_context_s(const node_state_s& state, const node_map_t& graph, update_t update, start_t& start)
        : state_(state)
        , graph_(graph)
        , update_(std::move(update))
        , start_(start)
    {
    }
    const node_state_s&  state() const { return state_; }
    const node_state_s*  find_state(std::string_view id) const;
    set_options_result_s update_settings(const nlohmann::json& patch) { return update_(patch); }
    // The caller transfers the same action to this handler after unlocking.
    void defer(start_t start) { start_ = std::move(start); }
};
} // namespace miximus::nodes
