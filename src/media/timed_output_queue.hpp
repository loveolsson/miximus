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
    utils::flicks target_time;
    T             value;
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

    static bool precedes(const frame_t& lhs, const frame_t& rhs) noexcept { return lhs.target_time < rhs.target_time; }

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

    output_frame_selection_s<T> select(utils::flicks target_time)
    {
        const auto limit    = target_time + config_.early_tolerance;
        auto       selected = frames_.end();
        for (auto it = frames_.begin(); it != frames_.end() && it->target_time <= limit; ++it) {
            selected = it;
        }

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

    const timed_output_queue_metrics_s& metrics() const noexcept { return metrics_; }
    size_t                              queued() const noexcept { return frames_.size(); }
    size_t                              capacity() const noexcept { return config_.capacity; }
    std::optional<utils::flicks>        oldest_target_time() const noexcept
    {
        if (frames_.empty()) {
            return std::nullopt;
        }
        return frames_.front().target_time;
    }
};

} // namespace miximus::media
