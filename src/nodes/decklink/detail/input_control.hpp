#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace miximus::nodes::decklink::detail {

// Serializes DeckLink capture-control calls away from the render thread. The
// SDK may block while streams stop, buffers are returned, or hardware is
// removed; none of those operations may delay graph evaluation.
class input_control_s
{
    std::mutex                        mutex_;
    std::condition_variable           condition_;
    std::deque<std::function<void()>> tasks_;
    bool                              stopping_{};
    std::thread                       worker_;

    void run();

  public:
    input_control_s();
    ~input_control_s();

    input_control_s(const input_control_s&)            = delete;
    input_control_s& operator=(const input_control_s&) = delete;
    input_control_s(input_control_s&&)                 = delete;
    input_control_s& operator=(input_control_s&&)      = delete;

    void post(std::function<void()> task);
};

} // namespace miximus::nodes::decklink::detail
