#include "output_presenter.hpp"

#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/geometry.hpp"
#include "gpu/shader.hpp"
#include "gpu/sync.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "media/media_frame.hpp"
#include "media/output_timeline.hpp"
#include "media/source_clock.hpp"
#include "media/timed_output_queue.hpp"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

namespace miximus::nodes::screen::detail {
namespace {
constexpr size_t OUTPUT_QUEUE_HEADROOM        = 3;
constexpr size_t RETAINED_PROGRAM_FRAME_COUNT = 1;
constexpr size_t RENDER_PIPELINE_HEADROOM     = 2;

constexpr size_t get_queue_capacity(size_t buffer_frames) noexcept { return buffer_frames + OUTPUT_QUEUE_HEADROOM; }

constexpr size_t get_slot_count(size_t buffer_frames) noexcept
{
    return get_queue_capacity(buffer_frames) + RETAINED_PROGRAM_FRAME_COUNT + RENDER_PIPELINE_HEADROOM;
}

int64_t to_microseconds(utils::flicks value) noexcept
{
    return std::chrono::duration_cast<std::chrono::microseconds>(value).count();
}

struct presentation_clock_result_s
{
    utils::flicks::rep interval_count{1};
    utils::flicks      completion_interval{};
    double             refresh_hz{};
};

class presentation_clock_s
{
    bool          uses_nominal_cadence_;
    utils::flicks nominal_frame_duration_;

    media::source_clock_estimator_s output_clock_;
    uint64_t                        output_sequence_{};
    utils::flicks                   output_pts_{};
    utils::flicks                   previous_completion_;
    utils::flicks                   next_nominal_swap_;

    double recovered_rate() const noexcept
    {
        return uses_nominal_cadence_ ? 1.0 : output_clock_.recovered_rate().value_or(1.0);
    }

  public:
    presentation_clock_s(bool          uses_nominal_cadence,
                         utils::flicks nominal_frame_duration,
                         utils::flicks initial_completion)
        : uses_nominal_cadence_(uses_nominal_cadence)
        , nominal_frame_duration_(nominal_frame_duration)
        , previous_completion_(initial_completion)
        , next_nominal_swap_(initial_completion + nominal_frame_duration)
    {
        if (!uses_nominal_cadence_) {
            output_clock_.observe(
                {
                    .epoch    = 0,
                    .sequence = output_sequence_,
                    .pts      = output_pts_,
                    .duration = nominal_frame_duration_,
                },
                initial_completion);
        }
    }

    void wait_for_next() const
    {
        if (!uses_nominal_cadence_) {
            return;
        }
        const auto deadline = std::chrono::steady_clock::time_point{
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(next_nominal_swap_)};
        std::this_thread::sleep_until(deadline);
    }

    utils::flicks predicted_completion() const noexcept
    {
        if (uses_nominal_cadence_) {
            return next_nominal_swap_;
        }
        return output_clock_.map(output_pts_ + nominal_frame_duration_)
            .value_or(previous_completion_ + nominal_frame_duration_);
    }

    presentation_clock_result_s observe_completion(utils::flicks completion)
    {
        const auto completion_interval = completion - previous_completion_;
        const auto rate                = recovered_rate();
        const auto recovered_duration  = utils::flicks{
            static_cast<utils::flicks::rep>(std::llround(static_cast<double>(nominal_frame_duration_.count()) * rate))};
        const auto interval_count =
            uses_nominal_cadence_
                ? std::max<utils::flicks::rep>(
                      1,
                      1 + ((completion - next_nominal_swap_ + (nominal_frame_duration_ / 2)) / nominal_frame_duration_))
                : std::max<utils::flicks::rep>(1,
                                               (completion_interval + (recovered_duration / 2)) / recovered_duration);

        output_sequence_ += static_cast<uint64_t>(interval_count);
        output_pts_ += nominal_frame_duration_ * interval_count;
        if (uses_nominal_cadence_) {
            next_nominal_swap_ += nominal_frame_duration_ * interval_count;
        } else {
            output_clock_.observe(
                {
                    .epoch    = 0,
                    .sequence = output_sequence_,
                    .pts      = output_pts_,
                    .duration = nominal_frame_duration_,
                },
                completion);
        }
        previous_completion_ = completion;

        const auto seconds = utils::to_seconds(nominal_frame_duration_) * recovered_rate();
        return {
            .interval_count      = interval_count,
            .completion_interval = completion_interval,
            .refresh_hz          = seconds > 0.0 && std::isfinite(seconds) ? 1.0 / seconds : 0.0,
        };
    }
};
} // namespace

class output_presenter_s::impl_s
{
    struct frame_slot_s
    {
        std::unique_ptr<gpu::framebuffer_s> target;
        std::unique_ptr<gpu::sync_s>        ready;
    };

    struct retired_slot_s
    {
        size_t                       index{};
        std::unique_ptr<gpu::sync_s> released;
    };

    class slot_lease_s
    {
        impl_s*      impl_{};
        size_t       index_{};
        mutable bool presented_{};

        void release() noexcept
        {
            if (impl_ == nullptr) {
                return;
            }

            std::unique_ptr<gpu::sync_s> released;
            if (presented_ && !impl_->display_finished_.load()) {
                released = std::make_unique<gpu::sync_s>();
                gpu::context_s::flush();
            }
            impl_->retire(index_, std::move(released));
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
            , presented_(other.presented_)
        {
        }

        slot_lease_s& operator=(slot_lease_s&& other) noexcept
        {
            if (this != &other) {
                release();
                impl_      = std::exchange(other.impl_, nullptr);
                index_     = other.index_;
                presented_ = other.presented_;
            }
            return *this;
        }

        size_t index() const noexcept { return index_; }
        void   mark_presented() const noexcept { presented_ = true; }
    };

    using submitted_frame_s = media::output_frame_s<slot_lease_s>;

    mutable std::mutex              mutex_;
    std::condition_variable         condition_;
    std::deque<size_t>              free_slots_;
    std::deque<submitted_frame_s>   submitted_frames_;
    std::deque<retired_slot_s>      retired_slots_;
    std::vector<frame_slot_s>       slots_;
    std::unique_ptr<gpu::context_s> context_;
    std::thread                     thread_;
    std::atomic_bool                running_{false};
    std::atomic_bool                display_finished_{true};

    const size_t        buffer_frames_;
    const utils::flicks nominal_frame_duration_;

    std::atomic_uint64_t frames_submitted_;
    std::atomic_uint64_t queue_overflow_drops_;
    std::atomic_uint64_t timing_drops_;
    std::atomic_uint64_t frames_repeated_;
    std::atomic_uint64_t frames_missing_;
    std::atomic_uint64_t output_intervals_skipped_;
    std::atomic_uint64_t swaps_completed_;
    std::atomic_uint64_t render_acquire_misses_;
    std::atomic_size_t   queued_frames_;
    std::atomic_int64_t  output_latency_us_;
    std::atomic_int64_t  selection_offset_us_;
    std::atomic_int64_t  completion_interval_max_us_;
    std::atomic<double>  measured_refresh_hz_;
    std::atomic_bool     uses_nominal_cadence_;

    void retire(size_t index, std::unique_ptr<gpu::sync_s> released) noexcept
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

    void submit(size_t index, utils::flicks target_time)
    {
        auto& slot = slots_.at(index);
        slot.ready = std::make_unique<gpu::sync_s>();
        gpu::context_s::flush();

        {
            const std::scoped_lock lock(mutex_);
            submitted_frames_.push_back({.target_time = target_time, .value = slot_lease_s(this, index)});
        }
        ++frames_submitted_;
        condition_.notify_one();
    }

    void reclaim_retired()
    {
        const std::scoped_lock lock(mutex_);
        auto                   it = retired_slots_.begin();
        while (it != retired_slots_.end()) {
            if (it->released && !it->released->cpu_wait(std::chrono::nanoseconds::zero())) {
                ++it;
                continue;
            }

            auto& slot = slots_.at(it->index);
            slot.ready.reset();
            it->released.reset();
            free_slots_.push_back(it->index);
            it = retired_slots_.erase(it);
        }
    }

    void collect_submitted(media::timed_output_queue_s<slot_lease_s>* queue)
    {
        std::deque<submitted_frame_s> submitted;
        {
            const std::scoped_lock lock(mutex_);
            submitted.swap(submitted_frames_);
        }
        while (!submitted.empty()) {
            queue->push(std::move(submitted.front()));
            submitted.pop_front();
        }
    }

    void publish_queue_metrics(const media::timed_output_queue_s<slot_lease_s>& queue) noexcept
    {
        const auto& metrics   = queue.metrics();
        queue_overflow_drops_ = metrics.overflow_drops;
        timing_drops_         = metrics.selection_drops;
        frames_repeated_      = metrics.repeated;
        frames_missing_       = metrics.missing;
        queued_frames_        = queue.queued();
    }

    void draw(gpu::textured_quad_s* textured_quad, const media::output_frame_selection_s<slot_lease_s>& selection)
    {
        if (selection.frame == nullptr) {
            return;
        }

        const auto& lease = selection.frame->value;
        auto&       slot  = slots_.at(lease.index());
        if (selection.selection == media::output_frame_selection_e::new_frame && slot.ready) {
            slot.ready->gpu_wait();
        }

        const auto framebuffer_size = context_->get_framebuffer_size();
        glViewport(0, 0, framebuffer_size.x, framebuffer_size.y);
        glClearColor(0, 0, 0, 0);
        glClear(static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT) | static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT));
        textured_quad->draw(slot.target->texture(),
                            {
                                .pos  = {0,   1.0 },
                                .size = {1.0, -1.0},
        });
        lease.mark_presented();
    }

    utils::flicks swap_and_wait()
    {
        context_->swap_buffers();

        // Keep the established screen-output completion boundary. In
        // particular, an interval-zero X11/XWayland swap is not sufficient
        // evidence that the GL work which updates the visible back buffer has
        // completed. Waiting here only parks the dedicated presenter thread;
        // it can never stall graph rendering.
        gpu::sync_s swap_finished;
        gpu::context_s::flush();
        (void)swap_finished.cpu_wait(std::chrono::hours(1));

        ++swaps_completed_;
        return utils::flicks_now();
    }

    void wait_for_preroll(media::timed_output_queue_s<slot_lease_s>* queue)
    {
        while (running_.load() && queue->queued() < buffer_frames_) {
            collect_submitted(queue);
            publish_queue_metrics(*queue);
            if (queue->queued() >= buffer_frames_) {
                return;
            }
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [this] { return !running_.load() || !submitted_frames_.empty(); });
        }
    }

    std::optional<utils::flicks> present_preroll(media::timed_output_queue_s<slot_lease_s>* queue,
                                                 gpu::textured_quad_s*                      textured_quad,
                                                 media::output_timeline_s*                  timeline)
    {
        const auto oldest_target = queue->oldest_target_time();
        if (!running_.load() || !oldest_target.has_value()) {
            return std::nullopt;
        }

        const auto selection = queue->select(*oldest_target);
        publish_queue_metrics(*queue);
        draw(textured_quad, selection);
        const auto completion = swap_and_wait();
        output_latency_us_    = to_microseconds(timeline->align(completion, *oldest_target));
        return completion;
    }

    std::optional<media::output_frame_selection_s<slot_lease_s>>
    select_frame(media::timed_output_queue_s<slot_lease_s>* queue,
                 media::output_timeline_s*                  timeline,
                 utils::flicks                              predicted_completion)
    {
        auto program_target = timeline->program_target_time(predicted_completion);
        if (!program_target.has_value()) {
            return std::nullopt;
        }

        const auto oldest_queued = queue->oldest_target_time();
        if (queue->queued() == queue->capacity() && oldest_queued.has_value() &&
            *oldest_queued > *program_target + (nominal_frame_duration_ / 2)) {
            output_latency_us_ = to_microseconds(timeline->align(predicted_completion, *oldest_queued));
            program_target     = oldest_queued;
        }

        auto selection = queue->select(*program_target);
        publish_queue_metrics(*queue);
        if (selection.frame != nullptr) {
            selection_offset_us_ = to_microseconds(selection.frame->target_time - *program_target);
        }
        return selection;
    }

    void publish_clock_result(const presentation_clock_result_s& result) noexcept
    {
        completion_interval_max_us_ =
            std::max(completion_interval_max_us_.load(), to_microseconds(result.completion_interval));
        if (result.interval_count > 1) {
            output_intervals_skipped_.fetch_add(static_cast<uint64_t>(result.interval_count - 1));
        }
        if (result.refresh_hz > 0.0) {
            measured_refresh_hz_ = result.refresh_hz;
        }
    }

    void run()
    {
        {
            const gpu::context_scope_s context_scope(*context_);
            glEnable(GL_FRAMEBUFFER_SRGB);

            // Miximus deliberately uses X11/XWayland on Linux. Mutter may stop
            // presenting an occluded or borderless XWayland window while a
            // swap-interval wait continues to return normally. In that case swap
            // completion is not a usable display clock, so pace against the
            // monitor's nominal refresh period instead.
            const bool use_nominal_cadence = glfwGetPlatform() == GLFW_PLATFORM_X11;
            uses_nominal_cadence_          = use_nominal_cadence;
            glfwSwapInterval(use_nominal_cadence ? 0 : 1);

            media::timed_output_queue_s<slot_lease_s> queue({
                .capacity        = get_queue_capacity(buffer_frames_),
                .early_tolerance = nominal_frame_duration_ / 2,
            });
            media::output_timeline_s                  timeline;
            auto*                shader = context_->get_shader(gpu::shader_program_s::name_e::basic);
            gpu::textured_quad_s textured_quad(shader, gpu::textured_quad_s::uv_e::regular);

            wait_for_preroll(&queue);
            if (auto completion = present_preroll(&queue, &textured_quad, &timeline); completion.has_value()) {
                presentation_clock_s clock(use_nominal_cadence, nominal_frame_duration_, *completion);
                while (running_.load()) {
                    clock.wait_for_next();
                    collect_submitted(&queue);

                    auto selection = select_frame(&queue, &timeline, clock.predicted_completion());
                    if (!selection.has_value()) {
                        break;
                    }
                    draw(&textured_quad, *selection);
                    publish_clock_result(clock.observe_completion(swap_and_wait()));
                }
            }

            collect_submitted(&queue);
            gpu::context_s::finish();
        }
        display_finished_ = true;
    }

  public:
    impl_s(gpu::context_s* root_context, size_t buffer_frames, utils::flicks nominal_frame_duration)
        : slots_(get_slot_count(buffer_frames))
        , context_(gpu::context_s::create_unique_context(true, root_context))
        , buffer_frames_(buffer_frames)
        , nominal_frame_duration_(nominal_frame_duration)
    {
        if (buffer_frames == 0 || nominal_frame_duration <= utils::flicks::zero()) {
            throw std::invalid_argument("screen output timing settings must be positive");
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

    gpu::context_s* context() noexcept { return context_.get(); }

    void start()
    {
        if (thread_.joinable()) {
            return;
        }
        display_finished_ = false;
        running_          = true;
        thread_           = std::thread(&impl_s::run, this);
    }

    void request_stop() noexcept
    {
        running_ = false;
        condition_.notify_one();
    }

    bool stopped() const noexcept { return display_finished_.load(); }

    void stop()
    {
        if (!thread_.joinable()) {
            return;
        }
        request_stop();
        thread_.join();
        reclaim_retired();

        std::deque<submitted_frame_s> submitted;
        {
            const std::scoped_lock lock(mutex_);
            submitted.swap(submitted_frames_);
        }
        submitted.clear();

        retired_slots_.clear();
        for (auto& slot : slots_) {
            slot.ready.reset();
            slot.target.reset();
        }
    }

    std::optional<output_presenter_s::render_frame_s> try_acquire(gpu::vec2i_t dimensions)
    {
        reclaim_retired();

        size_t index{};
        {
            const std::scoped_lock lock(mutex_);
            if (free_slots_.empty()) {
                ++render_acquire_misses_;
                return std::nullopt;
            }
            index = free_slots_.front();
            free_slots_.pop_front();
        }

        auto& slot = slots_.at(index);
        if (!slot.target || slot.target->texture()->texture_dimensions() != dimensions) {
            slot.target = std::make_unique<gpu::framebuffer_s>(dimensions, gpu::texture_s::format_e::bgra_u8);
        }
        return output_presenter_s::render_frame_s(this, index);
    }

    void abandon(size_t index) noexcept { return_unsubmitted(index); }

    gpu::framebuffer_s* target(size_t index) noexcept { return slots_.at(index).target.get(); }

    output_presenter_metrics_s metrics() const
    {
        size_t free_slots{};
        size_t retiring_slots{};
        {
            const std::scoped_lock lock(mutex_);
            free_slots     = free_slots_.size();
            retiring_slots = retired_slots_.size();
        }
        return {
            .frames_submitted             = frames_submitted_.load(),
            .program_queue_overflow_drops = queue_overflow_drops_.load(),
            .program_timing_drops         = timing_drops_.load(),
            .program_frames_repeated      = frames_repeated_.load(),
            .program_frames_missing       = frames_missing_.load(),
            .output_intervals_skipped     = output_intervals_skipped_.load(),
            .swaps_completed              = swaps_completed_.load(),
            .render_acquire_misses        = render_acquire_misses_.load(),
            .queued_frames                = queued_frames_.load(),
            .slots                        = slots_.size(),
            .free_slots                   = free_slots,
            .retiring_slots               = retiring_slots,
            .output_latency_us            = output_latency_us_.load(),
            .program_selection_offset_us  = selection_offset_us_.load(),
            .completion_interval_max_us   = completion_interval_max_us_.load(),
            .measured_refresh_hz          = measured_refresh_hz_.load(),
            .uses_nominal_cadence         = uses_nominal_cadence_.load(),
        };
    }

    friend class output_presenter_s::render_frame_s;
};

output_presenter_s::render_frame_s::render_frame_s(impl_s* impl, size_t slot_index) noexcept
    : impl_(impl)
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

gpu::framebuffer_s* output_presenter_s::render_frame_s::target() const noexcept
{
    return impl_ != nullptr ? impl_->target(slot_index_) : nullptr;
}

void output_presenter_s::render_frame_s::submit(utils::flicks target_time)
{
    if (impl_ == nullptr) {
        throw std::logic_error("screen output frame was already submitted");
    }
    auto* impl = std::exchange(impl_, nullptr);
    impl->submit(slot_index_, target_time);
}

output_presenter_s::output_presenter_s(gpu::context_s* root_context,
                                       size_t          buffer_frames,
                                       utils::flicks   nominal_frame_duration)
    : impl_(std::make_unique<impl_s>(root_context, buffer_frames, nominal_frame_duration))
{
}

output_presenter_s::~output_presenter_s() = default;

gpu::context_s* output_presenter_s::context() noexcept { return impl_->context(); }

void output_presenter_s::start() { impl_->start(); }

void output_presenter_s::request_stop() noexcept { impl_->request_stop(); }

bool output_presenter_s::stopped() const noexcept { return impl_->stopped(); }

void output_presenter_s::stop() { impl_->stop(); }

std::optional<output_presenter_s::render_frame_s> output_presenter_s::try_acquire(gpu::vec2i_t dimensions)
{
    return impl_->try_acquire(dimensions);
}

output_presenter_metrics_s output_presenter_s::metrics() const { return impl_->metrics(); }

} // namespace miximus::nodes::screen::detail
