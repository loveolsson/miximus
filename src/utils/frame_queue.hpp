#pragma once
#include "flicks.hpp"

#include <deque>
#include <mutex>
#include <optional>
#include <tuple>

namespace miximus::utils {

template <typename T, typename Mtx = std::recursive_mutex, typename Pts = flicks>
class frame_queue_s
{
  public:
    struct record_s
    {
        T   frame;
        Pts pts;

        record_s(T&& _frame, Pts _pts)
            : frame(std::move(_frame))
            , pts(_pts)
        {
        }
    };

    struct stats_s
    {
        Pts    min;
        Pts    max;
        size_t size;
    }

    frame_queue_s()
    {
    }

    size_t size()
    {
        auto lock = get_lock();

        return frames_.size();
    }

    std::pair<std::optional<record_s>, size_t> pop_frame()
    {
        auto lock = get_lock();

        if (!frames_.empty()) {
            auto res = std::make_pair<std::optional<T>, size_t>{std::move(frames_.front()), frames_.size() - 1};

            frames_.pop_front();

            return res;
        }

        return {std::nullopt, 0};
    }

    std::pair<std::optional<record_s>, size_t> pop_frame_if_newer(Pts pts)
    {
        auto lock = get_lock();

        if (!frames_.empty() && frames_.front().pts >= pts) {
            auto res = std::make_pair<std::optional<T>, size_t>{std::move(frames_.front()), frames_.size() - 1};

            frames_.pop_front();

            return res;
        }

        return {std::nullopt, frames_.size()};
    }

    size_t push_frame(T&& t, Pts pts = Pts())
    {
        auto lock = get_lock();

        frames_.emplace_back(std::move(t), pts);

        return frames_.size();
    }

    stats_s get_stats()
    {
        auto lock = get_lock();

        stats_s res = {};

        if (!frames_.empty()) {
            res.size = frames_.size();
            res.min  = frames_.front().pts;
            res.max  = frames_.back().pts;

            assert(res.min <= res.max);
        }

        return res;
    }

    std::unique_lock<Mtx> get_lock() { return {mtx_}; }

  private:
    Mtx                  mtx_;
    std::deque<record_s> frames_;
};

} // namespace miximus::utils