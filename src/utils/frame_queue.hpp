#pragma once
#include "flicks.hpp"

#include <mutex>
#include <optional>
#include <queue>
#include <tuple>

namespace miximus::utils {

template <typename T, typename Mtx = std::mutex, typename Pts = flicks>
class frame_queue_s
{
  public:
    struct record_s
    {
        T   frame;
        Pts pts;

        record_s()
            : frame(T())
            , pts(Pts())
        {
        }

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
    };

    size_t size_while_lock_held() { return frames_.size(); }

    size_t size()
    {
        auto lock = get_lock();
        return size_while_lock_held();
    }

    std::pair<std::optional<record_s>, size_t> pop_frame()
    {
        auto lock = get_lock();

        if (!frames_.empty()) {
            auto res = std::make_pair<std::optional<record_s>, size_t>(std::move(frames_.front()), frames_.size() - 1);

            frames_.pop();

            return res;
        }

        return {std::nullopt, 0};
    }

    bool pop_frame(record_s* record, size_t* left_in_queue = nullptr)
    {
        auto lock = get_lock();
        bool res  = false;

        if (!frames_.empty()) {
            if (record != nullptr) {
                *record = std::move(frames_.front());
            }

            res = true;

            frames_.pop();
        }

        if (left_in_queue != nullptr) {
            *left_in_queue = frames_.size();
        }

        return res;
    }

    std::pair<std::optional<record_s>, size_t> pop_frame_if_count(size_t count)
    {
        auto lock = get_lock();

        if (frames_.size() >= count) {
            auto res = std::make_pair<std::optional<record_s>, size_t>(std::move(frames_.front()), frames_.size() - 1);

            frames_.pop();

            return res;
        }

        return {std::nullopt, 0};
    }

    bool pop_frame_if_count(size_t count, record_s* record, size_t* left_in_queue = nullptr)
    {
        auto lock = get_lock();
        bool res  = false;

        if (frames_.size() >= count) {
            if (record != nullptr) {
                *record = std::move(frames_.front());
            }

            res = true;

            frames_.pop();
        }

        if (left_in_queue != nullptr) {
            *left_in_queue = frames_.size();
        }

        return res;
    }

    std::pair<std::optional<record_s>, size_t> pop_frame_if_older(Pts pts)
    {
        auto lock = get_lock();

        if (!frames_.empty() && frames_.front().pts <= pts) {
            auto res = std::make_pair<std::optional<record_s>, size_t>(std::move(frames_.front()), frames_.size() - 1);

            frames_.pop();

            return res;
        }

        return {std::nullopt, frames_.size()};
    }

    bool pop_frame_if_older(Pts pts, record_s* record, size_t* left_in_queue = nullptr)
    {
        auto lock = get_lock();
        bool res  = false;

        if (!frames_.empty() && frames_.front().pts <= pts) {
            if (record != nullptr) {
                *record = std::move(frames_.front());
            }

            res = true;

            frames_.pop();
        }

        if (left_in_queue != nullptr) {
            *left_in_queue = frames_.size();
        }

        return res;
    }

    size_t push_frame(T&& t, Pts pts = Pts())
    {
        auto lock = get_lock();

        frames_.emplace(std::move(t), pts);

        return frames_.size();
    }

    void clear()
    {
        auto lock = get_lock();
        frames_   = {};
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

    auto get_lock() { return std::unique_lock(mtx_); }

  private:
    Mtx                  mtx_;
    std::queue<record_s> frames_;
};

} // namespace miximus::utils