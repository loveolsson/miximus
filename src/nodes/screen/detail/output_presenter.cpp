#include "output_presenter.hpp"

#include "gpu/geometry.hpp"
#include "gpu/presenter.hpp"
#include "gpu/texture.hpp"
#include "gpu/window.hpp"
#include "logger/logger.hpp"
#include "media/playout_timeline.hpp"
#include "media/presentation_clock.hpp"
#include "media/timed_output_queue.hpp"
#include "types/output_buffer_limits.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace miximus::nodes::screen::detail {
namespace {
constexpr size_t OUTPUT_QUEUE_HEADROOM        = 3;
constexpr size_t RETAINED_PROGRAM_FRAME_COUNT = 1;
constexpr size_t RENDER_PIPELINE_HEADROOM     = 2;

size_t validate_buffer_frame_count(size_t buffer_frames)
{
    if (std::cmp_less(buffer_frames, screen_output_buffer_limits_s::MINIMUM_FRAME_COUNT) ||
        std::cmp_greater(buffer_frames, screen_output_buffer_limits_s::MAXIMUM_FRAME_COUNT)) {
        throw std::invalid_argument("screen output buffer frame count is outside its supported range");
    }
    return buffer_frames;
}

size_t get_queue_capacity(size_t buffer_frames)
{
    return validate_buffer_frame_count(buffer_frames) + OUTPUT_QUEUE_HEADROOM;
}

size_t get_slot_count(size_t buffer_frames)
{
    return get_queue_capacity(buffer_frames) + RETAINED_PROGRAM_FRAME_COUNT + RENDER_PIPELINE_HEADROOM;
}

int64_t to_microseconds(utils::flicks value) noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
}

} // namespace

class output_presenter_s::impl_s : public std::enable_shared_from_this<impl_s>
{
    struct frame_slot_s
    {
        std::unique_ptr<gpu::texture_s> target;
        gpu::completion_s               ready;
    };

    struct retired_slot_s
    {
        size_t            index{};
        gpu::completion_s released;
    };

    class slot_lease_s
    {
        impl_s* impl_{};
        size_t  index_{};

        void release() noexcept
        {
            if (impl_ == nullptr) {
                return;
            }

            impl_->retire(index_, {});
            impl_ = nullptr;
        }

      public:
        slot_lease_s() = default;
        slot_lease_s(impl_s* impl, size_t index) noexcept
            : impl_(impl)
            , index_(index)
        {
        }

        ~slot_lease_s() { release(); }

        slot_lease_s(const slot_lease_s&)            = delete;
        slot_lease_s& operator=(const slot_lease_s&) = delete;

        slot_lease_s(slot_lease_s&& other) noexcept
            : impl_(std::exchange(other.impl_, nullptr))
            , index_(other.index_)
        {
        }

        slot_lease_s& operator=(slot_lease_s&& other) noexcept
        {
            if (this != &other) {
                release();
                impl_  = std::exchange(other.impl_, nullptr);
                index_ = other.index_;
            }
            return *this;
        }

        size_t index() const noexcept { return index_; }
    };

    using lease_ptr         = std::shared_ptr<slot_lease_s>;
    using submitted_frame_s = media::output_frame_s<lease_ptr>;

    mutable std::mutex                mutex_;
    std::deque<size_t>                free_slots_;
    std::deque<submitted_frame_s>     submitted_frames_;
    std::deque<retired_slot_s>        retired_slots_;
    std::vector<frame_slot_s>         slots_;
    std::unique_ptr<gpu::window_s>    window_;
    gpu::device_s&                    device_;
    std::unique_ptr<gpu::presenter_s> presenter_;
    gpu::vec2i_t                      output_dimensions_{};
    std::atomic_bool                  running_{false};
    std::atomic_bool                  output_dimensions_changed_{false};

    const size_t        buffer_frames_;
    const utils::flicks nominal_frame_duration_;

    std::atomic_uint64_t                                  frames_submitted_;
    std::atomic_uint64_t                                  swaps_completed_;
    std::atomic_uint64_t                                  queue_overflow_drops_;
    std::atomic_uint64_t                                  timing_drops_;
    std::atomic_uint64_t                                  frames_repeated_;
    std::atomic_uint64_t                                  frames_missing_;
    std::atomic_uint64_t                                  output_intervals_skipped_;
    std::atomic_uint64_t                                  render_acquire_misses_;
    std::atomic_size_t                                    queued_frames_;
    std::atomic_int64_t                                   output_latency_us_;
    std::atomic_int64_t                                   selection_offset_us_;
    std::atomic_int64_t                                   completion_interval_max_us_;
    std::atomic<double>                                   measured_refresh_hz_;
    std::optional<media::timed_output_queue_s<lease_ptr>> queue_;
    media::presentation_timeline_s                        observed_latency_;
    media::playout_timeline_s                             timeline_;
    std::optional<media::presentation_clock_s>            presentation_clock_;
    std::optional<utils::flicks>                          presented_program_target_;
    std::optional<utils::flicks>                          scheduled_presentation_;
    std::atomic_bool                                      uses_present_wait_;

    std::mutex                  cadence_mutex_;
    std::condition_variable_any cadence_wake_;

    void retire(size_t index, gpu::completion_s released) noexcept
    {
        try {
            const std::scoped_lock lock(mutex_);
            retired_slots_.push_back({.index = index, .released = std::move(released)});
        } catch (...) {
            std::terminate();
        }
    }

    void return_unsubmitted(size_t index) noexcept
    {
        const std::scoped_lock lock(mutex_);
        free_slots_.push_back(index);
    }

    void submit(size_t index, utils::flicks program_target_time, gpu::completion_s ready)
    {
        const std::scoped_lock lock(mutex_);
        if (!running_.load()) {
            return;
        }
        auto& slot = slots_.at(index);
        slot.ready = std::move(ready);
        submitted_frames_.push_back(
            {.program_target_time = program_target_time, .payload = std::make_shared<slot_lease_s>(this, index)});
        ++frames_submitted_;
    }

    void reclaim_retired()
    {
        const std::scoped_lock lock(mutex_);
        auto                   it = retired_slots_.begin();
        while (it != retired_slots_.end()) {
            if (!slots_.at(it->index).target->idle()) {
                ++it;
                continue;
            }

            auto& slot   = slots_.at(it->index);
            slot.ready   = {};
            it->released = {};
            free_slots_.push_back(it->index);
            it = retired_slots_.erase(it);
        }
    }

    void collect_submitted(media::timed_output_queue_s<lease_ptr>& queue)
    {
        reclaim_retired();
        std::deque<submitted_frame_s> submitted;
        {
            const std::scoped_lock lock(mutex_);
            // Submission precedes publication. Select the exact program frame
            // even if its GPU work is unfinished; only the presenter waits.
            submitted.swap(submitted_frames_);
        }

        for (auto& frame : submitted) {
            queue.push(std::move(frame));
        }
    }

    void publish_queue_metrics(const media::timed_output_queue_s<lease_ptr>& queue) noexcept
    {
        const auto& metrics   = queue.metrics();
        queue_overflow_drops_ = metrics.overflow_drops;
        timing_drops_         = metrics.selection_drops;
        frames_repeated_      = metrics.repeated;
        frames_missing_       = metrics.missing;
        queued_frames_        = queue.queued();
    }

    std::optional<gpu::presentation_frame_s> next_frame(gpu::presentation_pacing_e pacing, const std::stop_token& stop)
    {
        if (!running_.load()) {
            return std::nullopt;
        }

        if (!queue_) {
            throw std::logic_error("Running screen presenter has no frame queue");
        }
        uses_present_wait_ = pacing == gpu::presentation_pacing_e::display;
        if (presentation_clock_ && !uses_present_wait_) {
            const auto deadline =
                std::chrono::steady_clock::time_point{std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                    presentation_clock_->predicted_completion() - nominal_frame_duration_ / 2)};
            std::unique_lock lock(cadence_mutex_);
            cadence_wake_.wait_until(lock, stop, deadline, [] { return false; });
            if (stop.stop_requested()) {
                return std::nullopt;
            }
        }

        auto& queue = *queue_;
        collect_submitted(queue);
        if (!timeline_.initialized() && queue.queued() < buffer_frames_) {
            publish_queue_metrics(queue);
            return std::nullopt;
        }

        const auto    oldest = queue.oldest_program_target_time();
        utils::flicks target{};
        if (!presentation_clock_) {
            if (!oldest) {
                throw std::logic_error("Primed screen queue has no frame timestamp");
            }
            target = *oldest;
        } else {
            const auto presentation = presentation_clock_->predicted_completion();
            target                  = timeline_.program_target(presentation);
            if (queue.queued() == queue.capacity() && oldest && *oldest > target + nominal_frame_duration_ / 2) {
                // Preserve the original full-queue fallback for a target that
                // has fallen entirely behind the retained program frames.
                timeline_.initialize(presentation, *oldest);
                target = timeline_.program_target(presentation);
            }
        }

        const auto selection = queue.select_nearest(target);
        publish_queue_metrics(queue);
        if (selection.frame == nullptr) {
            return std::nullopt;
        }

        selection_offset_us_      = to_microseconds(selection.frame->program_target_time - target);
        presented_program_target_ = selection.frame->program_target_time;
        scheduled_presentation_ =
            presentation_clock_ ? std::optional{presentation_clock_->predicted_completion()} : std::nullopt;
        const auto& lease = selection.frame->payload;
        const auto& slot  = slots_.at(lease->index());
        return gpu::presentation_frame_s{.image = *slot.target, .ready = slot.ready, .lease = lease};
    }

    void presentation_complete(gpu::presentation_feedback_s feedback)
    {
        const auto completion = feedback.time;
        uses_present_wait_    = feedback.timing == gpu::presentation_timing_e::present_wait;
        ++swaps_completed_;
        if (!presented_program_target_) {
            throw std::logic_error("Screen presentation completed without a selected frame");
        }

        output_latency_us_ = to_microseconds(observed_latency_.observe_latency(completion, *presented_program_target_));

        if (!presentation_clock_) {
            timeline_.initialize(completion, *presented_program_target_);
            const auto mode = uses_present_wait_ ? media::presentation_clock_mode_e::observed
                                                 : media::presentation_clock_mode_e::nominal;
            presentation_clock_.emplace(mode, nominal_frame_duration_, completion);
            measured_refresh_hz_ = 1.0 / utils::to_seconds(nominal_frame_duration_);
            return;
        }

        if (scheduled_presentation_) {
            timeline_.observe(*scheduled_presentation_, completion);
        }
        presented_program_target_.reset();

        const auto result = presentation_clock_->observe_completion(completion);
        completion_interval_max_us_ =
            std::max(completion_interval_max_us_.load(), to_microseconds(result.completion_interval));
        measured_refresh_hz_ = result.refresh_hz;
        if (result.interval_count > 1) {
            output_intervals_skipped_.fetch_add(static_cast<uint64_t>(result.interval_count - 1));
        }
    }

  public:
    impl_s(gpu::device_s&      device,
           size_t              buffer_frames,
           utils::flicks       nominal_frame_duration,
           bool                fullscreen,
           std::string_view    monitor_id,
           const gpu::recti_s& window_rect)
        : slots_(get_slot_count(buffer_frames))
        , window_(std::make_unique<gpu::window_s>(gpu::window_s::window_settings_s{.fullscreen = fullscreen,
                                                                                   .monitor_id = monitor_id,
                                                                                   .rect       = window_rect}))
        , device_(device)
        , output_dimensions_(window_->get_framebuffer_size())
        , buffer_frames_(buffer_frames)
        , nominal_frame_duration_(nominal_frame_duration)
        , queue_(std::in_place, media::timed_output_queue_config_s{.capacity = get_queue_capacity(buffer_frames)})
    {
        if (buffer_frames == 0 || nominal_frame_duration <= utils::flicks::zero() || output_dimensions_.x <= 0 ||
            output_dimensions_.y <= 0) {
            throw std::invalid_argument("screen output timing and drawable dimensions must be positive");
        }
        for (size_t index = 0; index < slots_.size(); ++index) {
            free_slots_.push_back(index);
        }
    }

    ~impl_s() { stop(); }

    impl_s(const impl_s&)            = delete;
    impl_s& operator=(const impl_s&) = delete;
    impl_s(impl_s&&)                 = delete;
    impl_s& operator=(impl_s&&)      = delete;

    void start()
    {
        if (presenter_) {
            return;
        }

        running_   = true;
        presenter_ = std::make_unique<gpu::presenter_s>(
            device_,
            window_->native_window(),
            gpu::extent_s{.width  = static_cast<uint32_t>(output_dimensions_.x),
                          .height = static_cast<uint32_t>(output_dimensions_.y)},
            gpu::presentation_source_s{
                .next_frame = [this](gpu::presentation_pacing_e pacing,
                                     const std::stop_token&     stop) { return next_frame(pacing, stop); },
                .complete   = [this](gpu::presentation_feedback_s feedback) { presentation_complete(feedback); },
            });
    }

    void request_stop() noexcept
    {
        running_ = false;
        if (presenter_) {
            presenter_->request_stop();
        }
    }

    bool stopped() const noexcept { return !presenter_ || presenter_->metrics().stopped; }

    void stop()
    {
        request_stop();
        presenter_.reset();
        // The callback and its final GPU copy have retired before releasing
        // the timed queue's leases or any of their owning render slots.
        queue_.reset();
        std::deque<submitted_frame_s> submitted;
        {
            const std::scoped_lock lock(mutex_);
            submitted.swap(submitted_frames_);
        }
        submitted.clear();
        const std::scoped_lock lock(mutex_);
        retired_slots_.clear();
        for (auto& slot : slots_) {
            slot.ready = {};
            slot.target.reset();
        }
    }

    void close_window() { window_.reset(); }

    gpu::vec2i_t output_dimensions() const noexcept { return output_dimensions_; }

    bool output_dimensions_changed() const noexcept { return output_dimensions_changed_.load(); }

    std::optional<output_presenter_s::render_frame_s> try_acquire()
    {
        if (window_->get_framebuffer_size() != output_dimensions_) {
            output_dimensions_changed_ = true;
            request_stop();
            return std::nullopt;
        }

        size_t index{};
        {
            const std::scoped_lock lock(mutex_);
            const auto             available = std::ranges::find_if(free_slots_, [this](size_t candidate) {
                return !slots_[candidate].target || slots_[candidate].target->idle();
            });
            if (available == free_slots_.end()) {
                ++render_acquire_misses_;
                return std::nullopt;
            }

            index = *available;
            free_slots_.erase(available);
        }

        auto& slot = slots_.at(index);
        if (!slot.target) {
            slot.target = std::make_unique<gpu::texture_s>(device_,
                                                           output_dimensions_,
                                                           gpu::format_e::rgba_unorm8,
                                                           gpu::channel_order_e::rgba,
                                                           gpu::sampling_e::linear);
        }
        return output_presenter_s::render_frame_s(shared_from_this(), index);
    }

    void abandon(size_t index) noexcept { return_unsubmitted(index); }

    gpu::texture_s* target(size_t index) noexcept { return slots_.at(index).target.get(); }

    output_presenter_metrics_s metrics() const
    {
        size_t free_slots{};
        size_t retiring_slots{};
        {
            const std::scoped_lock lock(mutex_);
            free_slots     = free_slots_.size();
            retiring_slots = retired_slots_.size();
        }
        const auto presentation = presenter_ ? presenter_->metrics() : gpu::presentation_metrics_s{};
        return {
            .failure                      = presentation.failure,
            .stopped                      = presentation.stopped,
            .frames_submitted             = frames_submitted_.load(),
            .program_queue_overflow_drops = queue_overflow_drops_.load(),
            .program_timing_drops         = timing_drops_.load(),
            .program_frames_repeated      = frames_repeated_.load(),
            .program_frames_missing       = frames_missing_.load(),
            .output_intervals_skipped     = output_intervals_skipped_.load(),
            .swaps_completed              = swaps_completed_.load(),
            .presentation_drops           = 0,
            .render_acquire_misses        = render_acquire_misses_.load(),
            .queued_frames                = queued_frames_.load(),
            .slots                        = slots_.size(),
            .free_slots                   = free_slots,
            .retiring_slots               = retiring_slots,
            .output_latency_us            = output_latency_us_.load(),
            .program_selection_offset_us  = selection_offset_us_.load(),
            .completion_interval_max_us   = completion_interval_max_us_.load(),
            .measured_refresh_hz          = measured_refresh_hz_.load(),
            .uses_present_wait            = uses_present_wait_.load(),
        };
    }

    friend class output_presenter_s::render_frame_s;
};

output_presenter_s::render_frame_s::render_frame_s(std::shared_ptr<impl_s> impl, size_t slot_index) noexcept
    : impl_(std::move(impl))
    , slot_index_(slot_index)
{
}

output_presenter_s::render_frame_s::~render_frame_s()
{
    if (impl_ != nullptr) {
        impl_->abandon(slot_index_);
    }
}

output_presenter_s::render_frame_s::render_frame_s(render_frame_s&& other) noexcept
    : impl_(std::exchange(other.impl_, nullptr))
    , slot_index_(other.slot_index_)
{
}

output_presenter_s::render_frame_s& output_presenter_s::render_frame_s::operator=(render_frame_s&& other) noexcept
{
    if (this != &other) {
        if (impl_ != nullptr) {
            impl_->abandon(slot_index_);
        }
        impl_       = std::exchange(other.impl_, nullptr);
        slot_index_ = other.slot_index_;
    }
    return *this;
}

gpu::texture_s* output_presenter_s::render_frame_s::target() const noexcept
{
    return impl_ != nullptr ? impl_->target(slot_index_) : nullptr;
}

void output_presenter_s::render_frame_s::submit(utils::flicks program_target_time, gpu::completion_s ready)
{
    if (impl_ == nullptr) {
        throw std::logic_error("screen output frame was already submitted");
    }
    auto impl = std::exchange(impl_, nullptr);
    impl->submit(slot_index_, program_target_time, std::move(ready));
}

output_presenter_s::output_presenter_s(gpu::device_s&      device,
                                       size_t              buffer_frames,
                                       utils::flicks       nominal_frame_duration,
                                       bool                fullscreen,
                                       std::string_view    monitor_id,
                                       const gpu::recti_s& window_rect)
    : impl_(
          std::make_shared<impl_s>(device, buffer_frames, nominal_frame_duration, fullscreen, monitor_id, window_rect))
{
}

output_presenter_s::~output_presenter_s()
{
    impl_->stop();
    // Window destruction belongs to the main thread. Pending publication leases
    // may retain only the stopped slot state beyond this point.
    impl_->close_window();
}

void output_presenter_s::start() { impl_->start(); }

void output_presenter_s::request_stop() noexcept { impl_->request_stop(); }

bool output_presenter_s::stopped() const noexcept { return impl_->stopped(); }

void output_presenter_s::stop() { impl_->stop(); }

gpu::vec2i_t output_presenter_s::output_dimensions() const noexcept { return impl_->output_dimensions(); }

bool output_presenter_s::output_dimensions_changed() const noexcept { return impl_->output_dimensions_changed(); }

std::optional<output_presenter_s::render_frame_s> output_presenter_s::try_acquire() { return impl_->try_acquire(); }

output_presenter_metrics_s output_presenter_s::metrics() const { return impl_->metrics(); }

} // namespace miximus::nodes::screen::detail
