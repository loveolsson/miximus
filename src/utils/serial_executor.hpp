#pragma once

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <utility>

namespace miximus::utils {

// Runs infrequent, potentially blocking control operations in submission
// order. Destruction rejects new work, drains accepted work, and joins the
// worker thread.
class serial_executor_s
{
    std::mutex                        mutex_;
    std::condition_variable           condition_;
    std::deque<std::function<void()>> tasks_;
    bool                              closing_{};
    std::thread                       worker_;

    void run()
    {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [this] { return closing_ || !tasks_.empty(); });
                if (tasks_.empty()) {
                    return;
                }
                task = std::move(tasks_.front());
                tasks_.pop_front();
            }
            task();
        }
    }

  public:
    serial_executor_s()
        : worker_([this] { run(); })
    {
    }

    ~serial_executor_s()
    {
        {
            const std::scoped_lock lock(mutex_);
            closing_ = true;
        }
        condition_.notify_one();
        worker_.join();
    }

    serial_executor_s(const serial_executor_s&)            = delete;
    serial_executor_s& operator=(const serial_executor_s&) = delete;
    serial_executor_s(serial_executor_s&&)                 = delete;
    serial_executor_s& operator=(serial_executor_s&&)      = delete;

    bool post(std::function<void()> task)
    {
        {
            const std::scoped_lock lock(mutex_);
            if (closing_) {
                return false;
            }
            tasks_.emplace_back(std::move(task));
        }
        condition_.notify_one();
        return true;
    }
};

} // namespace miximus::utils
