#include "input_control.hpp"

#include "logger/logger.hpp"

#include <exception>
#include <utility>

namespace miximus::nodes::decklink::detail {

input_control_s::input_control_s()
    : worker_([this] { run(); })
{
}

input_control_s::~input_control_s()
{
    {
        const std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    condition_.notify_one();
    worker_.join();
}

void input_control_s::post(std::function<void()> task)
{
    {
        const std::scoped_lock lock(mutex_);
        if (stopping_) {
            return;
        }
        tasks_.emplace_back(std::move(task));
    }
    condition_.notify_one();
}

void input_control_s::run()
{
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        try {
            task();
        } catch (const std::exception& error) {
            getlog("decklink")->error("DeckLink input control task failed: {}", error.what());
        } catch (...) {
            getlog("decklink")->error("DeckLink input control task failed");
        }
    }
}

} // namespace miximus::nodes::decklink::detail
