#pragma once
#include "media/media_frame.hpp"

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
    media_frame_id_s id;
    T                value;
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
    uint64_t discontinuities{};
};

template <typename T>
class timed_output_queue_s
{
    using frame_t = output_frame_s<T>;

    timed_output_queue_config_s  config_;
    std::deque<frame_t>          frames_;
    std::optional<frame_t>       current_;
    std::optional<uint64_t>      epoch_;
    timed_output_queue_metrics_s metrics_;

    static bool precedes(const frame_t& lhs, const frame_t& rhs)
    {
        if (lhs.id.epoch != rhs.id.epoch) {
            return lhs.id.epoch < rhs.id.epoch;
        }
        if (lhs.id.pts != rhs.id.pts) {
            return lhs.id.pts < rhs.id.pts;
        }
        return lhs.id.sequence < rhs.id.sequence;
    }

    void clear_frames()
    {
        frames_.clear();
        current_.reset();
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
        if (current_.has_value() && frame.id.epoch == current_->id.epoch && !precedes(*current_, frame)) {
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

    output_frame_selection_s<T> select(uint64_t epoch, utils::flicks pts)
    {
        if (epoch_.has_value() && *epoch_ != epoch) {
            clear_frames();
            ++metrics_.discontinuities;
        }
        epoch_ = epoch;

        std::erase_if(frames_, [epoch](const frame_t& frame) { return frame.id.epoch != epoch; });

        const auto limit    = pts + config_.early_tolerance;
        auto       selected = frames_.end();
        for (auto it = frames_.begin(); it != frames_.end() && it->id.pts <= limit; ++it) {
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

    void reset()
    {
        clear_frames();
        epoch_.reset();
        ++metrics_.discontinuities;
    }

    const timed_output_queue_metrics_s& metrics() const { return metrics_; }
};

} // namespace miximus::media
