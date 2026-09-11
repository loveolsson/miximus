#pragma once
#include "utils/flicks.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <optional>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace miximus::media {

enum class output_frame_selection_e : uint8_t
{
    new_frame,
    repeat,
    missing,
};

template <typename T>
struct output_frame_s
{
    utils::flicks program_target_time;
    T             payload;
};

template <typename T>
struct output_frame_selection_s
{
    output_frame_selection_e selection{output_frame_selection_e::missing};
    const output_frame_s<T>* frame{};
};

struct timed_output_queue_config_s
{
    size_t        capacity{8};
    utils::flicks early_tolerance{};
};

struct timed_output_queue_metrics_s
{
    uint64_t pushed{};
    uint64_t overflow_drops{};
    uint64_t selection_drops{};
    uint64_t repeated{};
    uint64_t missing{};
};

template <typename T>
class timed_output_queue_s
{
    using frame_t = output_frame_s<T>;

    timed_output_queue_config_s  config_;
    std::deque<frame_t>          frames_;
    std::optional<frame_t>       current_;
    timed_output_queue_metrics_s metrics_;

    static bool precedes(const frame_t& lhs, const frame_t& rhs) noexcept
    {
        return lhs.program_target_time < rhs.program_target_time;
    }

    output_frame_selection_s<T> select_frame(typename std::deque<frame_t>::iterator selected)
    {
        if (selected != frames_.end()) {
            metrics_.selection_drops += static_cast<uint64_t>(std::distance(frames_.begin(), selected));
            current_ = std::move(*selected);
            frames_.erase(frames_.begin(), std::next(selected));
            return {.selection = output_frame_selection_e::new_frame, .frame = &*current_};
        }

        if (current_.has_value()) {
            ++metrics_.repeated;
            return {.selection = output_frame_selection_e::repeat, .frame = &*current_};
        }

        ++metrics_.missing;
        return {};
    }

  public:
    explicit timed_output_queue_s(timed_output_queue_config_s config = {})
        : config_(config)
    {
        if (config_.capacity == 0) {
            throw std::invalid_argument("timed output queue capacity must be positive");
        }
    }

    timed_output_queue_s(const timed_output_queue_s&)            = delete;
    timed_output_queue_s(timed_output_queue_s&&)                 = delete;
    timed_output_queue_s& operator=(const timed_output_queue_s&) = delete;
    timed_output_queue_s& operator=(timed_output_queue_s&&)      = delete;

    void push(frame_t frame)
    {
        if (current_.has_value() && !precedes(*current_, frame)) {
            ++metrics_.selection_drops;
            return;
        }

        const auto position = std::ranges::upper_bound(frames_, frame, precedes);
        frames_.insert(position, std::move(frame));
        ++metrics_.pushed;

        while (frames_.size() > config_.capacity) {
            frames_.pop_front();
            ++metrics_.overflow_drops;
        }
    }

    [[nodiscard]] output_frame_selection_s<T> select(utils::flicks program_target_time)
    {
        const auto limit    = program_target_time + config_.early_tolerance;
        auto       selected = frames_.end();
        for (auto it = frames_.begin(); it != frames_.end() && it->program_target_time <= limit; ++it) {
            selected = it;
        }

        return select_frame(selected);
    }

    // Screen playout chooses the closest PTS, including the retained image.
    // Ties keep the older image; queued future images remain available.
    [[nodiscard]] output_frame_selection_s<T> select_nearest(utils::flicks program_target_time)
    {
        auto selected = frames_.end();
        auto distance =
            current_ ? std::chrono::abs(current_->program_target_time - program_target_time) : utils::flicks::max();
        for (auto it = frames_.begin(); it != frames_.end(); ++it) {
            const auto candidate_distance = std::chrono::abs(it->program_target_time - program_target_time);
            if (candidate_distance < distance) {
                selected = it;
                distance = candidate_distance;
            }
        }
        return select_frame(selected);
    }

    const timed_output_queue_metrics_s& metrics() const noexcept { return metrics_; }
    size_t                              queued() const noexcept { return frames_.size(); }
    size_t                              capacity() const noexcept { return config_.capacity; }
    std::optional<utils::flicks>        oldest_program_target_time() const noexcept
    {
        if (frames_.empty()) {
            return std::nullopt;
        }
        return frames_.front().program_target_time;
    }
};

} // namespace miximus::media
