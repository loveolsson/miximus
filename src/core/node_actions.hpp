#pragma once

#include "core/node_handle.hpp"
#include "nodes/action.hpp"
#include "nodes/node_map.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace miximus::core {

// Transient part of a pending graph update. Admission and frame handoff use the
// graph lock, with no independent inbox or snapshot cutoff.
class node_actions_s
{
  public:
    using clock_t                        = std::chrono::steady_clock;
    using reply_t                        = nodes::action_s::reply_t;
    static constexpr size_t MAX_PER_NODE = 64;
    static constexpr auto   MAX_AGE      = std::chrono::seconds(5);

    using request_s = nodes::action_s;
    struct pending_s
    {
        // An entry also marks this node's settings/connections dirty.
        std::deque<request_s> actions;
    };
    class batch_t
    {
        std::vector<std::deque<request_s>> groups_;
        friend class node_manager_s;

      public:
        batch_t() = default;
        ~batch_t();
        batch_t(const batch_t&)            = delete;
        batch_t& operator=(const batch_t&) = delete;
        batch_t(batch_t&& other) noexcept;
        batch_t& operator=(batch_t&& other) noexcept;
        size_t   size() const;
        void cancel(error_e error = error_e::cancelled, std::string_view message = "Node action frame was abandoned");
        void dispatch(app_state_s* app, const nodes::node_map_t& snapshot, clock_t::time_point now = clock_t::now());
    };

    static bool valid_request(std::string_view id, std::string_view name);
};

} // namespace miximus::core
