#pragma once
#include "media/source_clock.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <ranges>
#include <stdexcept>
#include <utility>

namespace miximus::media {

enum class source_frame_readiness_e : uint8_t
{
    reserved,
    submitted,
    ready,
    failed,
};

template <typename T>
class source_frame_s
{
    std::atomic<source_frame_readiness_e> readiness_;
    T                                     value_;

  public:
    media_frame_id_s id;
    utils::flicks    arrival_time;

    source_frame_s(media_frame_id_s         frame_id,
                   utils::flicks            arrival,
                   T                        value,
                   source_frame_readiness_e readiness = source_frame_readiness_e::reserved)
        : readiness_(readiness)
        , value_(std::move(value))
        , id(frame_id)
        , arrival_time(arrival)
    {
    }

    source_frame_s(const source_frame_s&)            = delete;
    source_frame_s(source_frame_s&&)                 = delete;
    source_frame_s& operator=(const source_frame_s&) = delete;
    source_frame_s& operator=(source_frame_s&&)      = delete;

    source_frame_readiness_e readiness() const { return readiness_.load(std::memory_order_acquire); }
    const T&                 value() const { return value_; }
    T&                       value() { return value_; }

    bool mark_submitted()
    {
        auto       expected = source_frame_readiness_e::reserved;
        const bool changed  = readiness_.compare_exchange_strong(
            expected, source_frame_readiness_e::submitted, std::memory_order_release, std::memory_order_relaxed);
        if (changed) {
            readiness_.notify_all();
        }
        return changed;
    }

    bool mark_ready()
    {
        auto       expected = source_frame_readiness_e::submitted;
        const bool changed  = readiness_.compare_exchange_strong(
            expected, source_frame_readiness_e::ready, std::memory_order_release, std::memory_order_relaxed);
        if (changed) {
            readiness_.notify_all();
        }
        return changed;
    }

    bool mark_failed()
    {
        auto state = readiness();
        while (state != source_frame_readiness_e::ready && state != source_frame_readiness_e::failed) {
            if (readiness_.compare_exchange_weak(
                    state, source_frame_readiness_e::failed, std::memory_order_release, std::memory_order_acquire)) {
                readiness_.notify_all();
                return true;
            }
        }
        return false;
    }

    bool await() const
    {
        auto state = readiness();
        while (state != source_frame_readiness_e::ready && state != source_frame_readiness_e::failed) {
            readiness_.wait(state, std::memory_order_acquire);
            state = readiness();
        }
        return state == source_frame_readiness_e::ready;
    }
};

enum class prepared_frame_selection_e : uint8_t
{
    new_frame,
    repeat,
    missing,
};

template <typename T>
class timed_source_queue_s;

template <typename T>
class prepared_frame_ticket_s
{
  public:
    using frame_t     = source_frame_s<T>;
    using frame_ptr_t = std::shared_ptr<frame_t>;

  private:
    friend class timed_source_queue_s<T>;

    frame_ptr_t                frame_;
    prepared_frame_selection_e selection_{prepared_frame_selection_e::missing};
    bool                       discontinuity_{};

    prepared_frame_ticket_s(frame_ptr_t frame, prepared_frame_selection_e selection, bool discontinuity)
        : frame_(std::move(frame))
        , selection_(selection)
        , discontinuity_(discontinuity)
    {
    }

  public:
    prepared_frame_ticket_s() = default;

    const frame_ptr_t&         frame() const { return frame_; }
    prepared_frame_selection_e selection() const { return selection_; }
    bool                       discontinuity() const { return discontinuity_; }
    bool                       await() const { return frame_ != nullptr && frame_->await(); }
};

struct timed_source_queue_config_s
{
    size_t                capacity{8};
    utils::flicks         playout_delay{};
    utils::flicks         early_tolerance{};
    source_clock_config_s clock{};
};

struct timed_source_queue_metrics_s
{
    uint64_t                     pushed{};
    uint64_t                     overflow_drops{};
    uint64_t                     selection_drops{};
    uint64_t                     repeated{};
    uint64_t                     missing{};
    uint64_t                     discontinuities{};
    uint64_t                     transfer_failures{};
    std::optional<double>        recovered_rate;
    std::optional<utils::flicks> phase_offset;
};

template <typename T>
class timed_source_queue_s
{
  public:
    using frame_t     = source_frame_s<T>;
    using frame_ptr_t = std::shared_ptr<frame_t>;
    using ticket_t    = prepared_frame_ticket_s<T>;

  private:
    struct aligned_frame_s
    {
        frame_ptr_t   frame;
        utils::flicks program_pts;
    };

    timed_source_queue_config_s config_;
    source_clock_estimator_s    clock_;
    mutable std::mutex          pending_mutex_;
    std::deque<frame_ptr_t>     pending_;
    std::deque<aligned_frame_s> frames_;
    frame_ptr_t                 current_;
    bool                        discontinuity_pending_{};

    std::atomic_uint64_t pushed_{};
    std::atomic_uint64_t overflow_drops_{};
    std::atomic_uint64_t selection_drops_{};
    std::atomic_uint64_t repeated_{};
    std::atomic_uint64_t missing_{};
    std::atomic_uint64_t discontinuities_{};
    std::atomic_uint64_t transfer_failures_{};

    static void cancel_frame(const frame_ptr_t& frame)
    {
        if (frame != nullptr) {
            (void)frame->mark_failed();
        }
    }

    void clear_for_discontinuity(bool reset_clock)
    {
        if (reset_clock) {
            clock_.reset();
        }
        for (const auto& frame : frames_) {
            cancel_frame(frame.frame);
        }
        cancel_frame(current_);
        frames_.clear();
        current_.reset();
        discontinuity_pending_ = true;
        discontinuities_.fetch_add(1, std::memory_order_relaxed);
    }

  public:
    explicit timed_source_queue_s(timed_source_queue_config_s config = {})
        : config_(config)
        , clock_(config.clock)
    {
        if (config_.capacity == 0) {
            throw std::invalid_argument("timed source queue capacity must be positive");
        }
    }

    ~timed_source_queue_s()
    {
        {
            const std::scoped_lock lock(pending_mutex_);
            for (const auto& frame : pending_) {
                cancel_frame(frame);
            }
        }
        for (const auto& frame : frames_) {
            cancel_frame(frame.frame);
        }
        cancel_frame(current_);
    }

    timed_source_queue_s(const timed_source_queue_s&)            = delete;
    timed_source_queue_s(timed_source_queue_s&&)                 = delete;
    timed_source_queue_s& operator=(const timed_source_queue_s&) = delete;
    timed_source_queue_s& operator=(timed_source_queue_s&&)      = delete;

    frame_ptr_t create_frame(media_frame_id_s         id,
                             utils::flicks            arrival_time,
                             T                        value,
                             source_frame_readiness_e readiness = source_frame_readiness_e::reserved) const
    {
        return std::make_shared<frame_t>(id, arrival_time, std::move(value), readiness);
    }

    void push(frame_ptr_t frame)
    {
        if (frame == nullptr) {
            return;
        }

        const std::scoped_lock lock(pending_mutex_);
        pending_.push_back(std::move(frame));
        pushed_.fetch_add(1, std::memory_order_relaxed);
        while (pending_.size() > config_.capacity) {
            cancel_frame(pending_.front());
            pending_.pop_front();
            overflow_drops_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void advance(utils::flicks program_pts, utils::flicks program_target_time, bool program_discontinuity = false)
    {
        std::deque<frame_ptr_t> pending;
        {
            const std::scoped_lock lock(pending_mutex_);
            pending.swap(pending_);
        }

        if (program_discontinuity) {
            clear_for_discontinuity(true);
        }

        for (auto& frame : pending) {
            const auto program_observation = program_pts + frame->arrival_time - program_target_time;
            const auto observation         = clock_.observe(frame->id, program_observation);
            if (observation == source_clock_observation_e::discontinuity) {
                // observe() has already re-anchored the estimator to this frame.
                clear_for_discontinuity(false);
            }

            const auto mapped = clock_.map(frame->id.pts);
            if (!mapped.has_value()) {
                continue;
            }

            aligned_frame_s aligned{
                .frame       = std::move(frame),
                .program_pts = *mapped + config_.playout_delay,
            };
            const auto insertion =
                std::ranges::upper_bound(frames_, aligned.program_pts, {}, &aligned_frame_s::program_pts);
            frames_.insert(insertion, std::move(aligned));
            while (frames_.size() > config_.capacity) {
                cancel_frame(frames_.front().frame);
                frames_.pop_front();
                overflow_drops_.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    ticket_t select(utils::flicks program_pts)
    {
        const auto limit    = program_pts + config_.early_tolerance;
        auto       selected = frames_.end();
        for (auto it = frames_.begin(); it != frames_.end() && it->program_pts <= limit; ++it) {
            selected = it;
        }

        const bool discontinuity = std::exchange(discontinuity_pending_, false);
        if (selected != frames_.end()) {
            const auto dropped = static_cast<uint64_t>(std::distance(frames_.begin(), selected));
            selection_drops_.fetch_add(dropped, std::memory_order_relaxed);
            for (auto it = frames_.begin(); it != selected; ++it) {
                cancel_frame(it->frame);
            }
            frames_.erase(frames_.begin(), selected);
            return ticket_t(selected->frame, prepared_frame_selection_e::new_frame, discontinuity);
        }

        if (current_ != nullptr) {
            repeated_.fetch_add(1, std::memory_order_relaxed);
            return ticket_t(current_, prepared_frame_selection_e::repeat, discontinuity);
        }

        missing_.fetch_add(1, std::memory_order_relaxed);
        return ticket_t({}, prepared_frame_selection_e::missing, discontinuity);
    }

    bool commit(const ticket_t& ticket)
    {
        if (ticket.selection_ == prepared_frame_selection_e::repeat) {
            return ticket.frame_ != nullptr && ticket.frame_->readiness() == source_frame_readiness_e::ready;
        }
        if (ticket.selection_ != prepared_frame_selection_e::new_frame || ticket.frame_ == nullptr ||
            ticket.frame_->readiness() != source_frame_readiness_e::ready) {
            return false;
        }

        const auto match =
            std::ranges::find_if(frames_, [&](const aligned_frame_s& frame) { return frame.frame == ticket.frame_; });
        if (match == frames_.end()) {
            return false;
        }
        frames_.erase(frames_.begin(), std::next(match));
        current_ = ticket.frame_;
        return true;
    }

    void fail(const ticket_t& ticket)
    {
        if (ticket.selection_ != prepared_frame_selection_e::new_frame || ticket.frame_ == nullptr) {
            return;
        }

        if (!ticket.frame_->mark_failed()) {
            return;
        }
        const auto match =
            std::ranges::find_if(frames_, [&](const aligned_frame_s& frame) { return frame.frame == ticket.frame_; });
        if (match != frames_.end()) {
            frames_.erase(match);
        }
        transfer_failures_.fetch_add(1, std::memory_order_relaxed);
    }

    void reset()
    {
        {
            const std::scoped_lock lock(pending_mutex_);
            for (const auto& frame : pending_) {
                cancel_frame(frame);
            }
            pending_.clear();
        }
        clear_for_discontinuity(true);
    }

    timed_source_queue_metrics_s metrics() const
    {
        return {
            .pushed            = pushed_.load(std::memory_order_relaxed),
            .overflow_drops    = overflow_drops_.load(std::memory_order_relaxed),
            .selection_drops   = selection_drops_.load(std::memory_order_relaxed),
            .repeated          = repeated_.load(std::memory_order_relaxed),
            .missing           = missing_.load(std::memory_order_relaxed),
            .discontinuities   = discontinuities_.load(std::memory_order_relaxed),
            .transfer_failures = transfer_failures_.load(std::memory_order_relaxed),
            .recovered_rate    = clock_.recovered_rate(),
            .phase_offset      = clock_.phase_offset(),
        };
    }
};

} // namespace miximus::media
