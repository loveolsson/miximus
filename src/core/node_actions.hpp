#pragma once

#include "core/app_state_fwd.hpp"
#include "nodes/action.hpp"
#include "nodes/node_map.hpp"
#include "types/node_action_limits.hpp"

#include <chrono>
#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace miximus::core {

// Transport-independent, bounded delivery. Does not retain nodes or access live
// node state on the producer thread. Call take_batch with the graph snapshot lock.
class node_actions_s
{
  public:
    using clock_t                         = std::chrono::steady_clock;
    using reply_t                         = std::function<void(nodes::action_result_s)>;
    static constexpr size_t MAX_PENDING   = 64;
    static constexpr size_t MAX_PER_NODE  = 8;
    static constexpr size_t MAX_PER_FRAME = 16;
    static constexpr auto   MAX_AGE       = std::chrono::seconds(5);

    struct request_s
    {
        std::string                  id;
        std::weak_ptr<nodes::node_i> target;
        std::string                  name;
        nlohmann::json               payload;
        reply_t                      reply;
        clock_t::time_point          deadline;
    };
    class batch_t
    {
        std::vector<request_s> requests_;
        friend class node_actions_s;

      public:
        batch_t() = default;
        ~batch_t();
        batch_t(const batch_t&)            = delete;
        batch_t& operator=(const batch_t&) = delete;
        batch_t(batch_t&& other) noexcept { requests_.swap(other.requests_); }
        batch_t& operator=(batch_t&& other) noexcept;
        size_t   size() const { return requests_.size(); }
    };

  private:
    std::mutex            mutex_;
    std::deque<request_s> pending_;
    bool                  closed_{};

  public:
    ~node_actions_s() { close(); }

    // Rejection returns immediately without calling reply. Accepted requests
    // receive exactly one reply on dispatch or cancellation, outside all locks.
    error_e     enqueue(std::string_view             id,
                        std::weak_ptr<nodes::node_i> target,
                        std::string_view             name,
                        const nlohmann::json&        payload,
                        reply_t                      callback,
                        clock_t::time_point          now = clock_t::now());
    batch_t     take_batch();
    static void dispatch(batch_t                  batch,
                         app_state_s*             app,
                         const nodes::node_map_t& snapshot,
                         clock_t::time_point      now = clock_t::now());
    void        close();
};

} // namespace miximus::core
