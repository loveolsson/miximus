#include "output_sender.hpp"

#include "gpu/transfer/texture_readback.hpp"
#include "logger/logger.hpp"
#include "media/presentation_timeline.hpp"
#include "media/timed_output_queue.hpp"
#include "types/output_buffer_limits.hpp"
#include "utils/serial_executor.hpp"
#include "wrapper/ndi-sdk/ndi_inc.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace miximus::nodes::ndi::detail {
namespace {
auto log() { return getlog("ndi"); }

constexpr auto COLOR_METADATA = R"(<ndi_color_info primaries="bt_709" transfer="bt_709" matrix="bt_709"/>)";

struct sender_frame_s
{
    std::shared_ptr<gpu::transfer::texture_readback_frame_s> readback;
    gpu::vec2i_t                                             dimensions;
};
} // namespace

class output_sender_s::impl_s
{
    struct stream_state_s
    {
        std::shared_ptr<gpu::transfer::texture_readback_stream_s> stream;
        gpu::vec2i_t                                              dimensions{};
        frame_rate_s                                              frame_rate;
        utils::flicks                                             frame_duration{};
        utils::flicks                                             program_time_origin{};
        size_t                                                    buffer_frames{};
        uint64_t                                                  configuration_generation{};
    };

    utils::serial_executor_s* control_executor_;
    std::string               sender_name_;
    NDIlib_send_instance_t    sender_{nullptr};

    std::atomic<phase_e> phase_{phase_e::starting};
    std::atomic_bool     stop_requested_;

    mutable std::mutex            state_mutex_;
    std::condition_variable       state_condition_;
    std::optional<stream_state_s> stream_state_;
    uint64_t                      stream_configuration_generation_{};
    bool                          worker_running_{};
    std::thread                   worker_;

    std::atomic_uint64_t program_frames_received_;
    std::atomic_uint64_t program_queue_overflow_drops_;
    std::atomic_uint64_t program_timing_drops_;
    std::atomic_uint64_t program_frames_repeated_;
    std::atomic_uint64_t program_frames_missing_;
    std::atomic_uint64_t output_intervals_skipped_;
    std::atomic_uint64_t frames_sent_;
    std::atomic_size_t   queued_frames_;
    std::atomic_int64_t  output_latency_us_;
    std::atomic_int64_t  program_selection_offset_us_;

    static int64_t to_ndi_timecode(utils::flicks pts)
    {
        constexpr int64_t NDI_TICKS_PER_SECOND = 10'000'000;
        const auto        seconds              = pts.count() / utils::k_flicks_one_second.count();
        const auto        remainder            = pts.count() % utils::k_flicks_one_second.count();
        return (seconds * NDI_TICKS_PER_SECOND) +
               ((remainder * NDI_TICKS_PER_SECOND) / utils::k_flicks_one_second.count());
    }

    void publish_queue_metrics(const media::timed_output_queue_s<sender_frame_s>& queue)
    {
        const auto& metrics           = queue.metrics();
        program_frames_received_      = metrics.pushed;
        program_queue_overflow_drops_ = metrics.overflow_drops;
        program_timing_drops_         = metrics.selection_drops;
        program_frames_repeated_      = metrics.repeated;
        program_frames_missing_       = metrics.missing;
        queued_frames_                = queue.queued();
    }

    static std::chrono::steady_clock::duration steady_duration(utils::flicks duration)
    {
        return std::chrono::duration_cast<std::chrono::steady_clock::duration>(duration);
    }

    static std::chrono::steady_clock::time_point steady_time_point(utils::flicks time)
    {
        return std::chrono::steady_clock::time_point(steady_duration(time));
    }

    void send_frame(const stream_state_s& state, const sender_frame_s& frame, utils::flicks program_pts)
    {
        const auto bytes = frame.readback->readable_host_bytes();

        NDIlib_video_frame_v2_t ndi_frame{};
        ndi_frame.xres                 = frame.dimensions.x;
        ndi_frame.yres                 = frame.dimensions.y;
        ndi_frame.FourCC               = NDIlib_FourCC_video_type_RGBA;
        ndi_frame.line_stride_in_bytes = frame.dimensions.x * 4;
        ndi_frame.p_data               = ndi_sdk::send_buffer(bytes);
        ndi_frame.frame_rate_N         = static_cast<int>(state.frame_rate.numerator);
        ndi_frame.frame_rate_D         = static_cast<int>(state.frame_rate.denominator);
        ndi_frame.frame_format_type    = NDIlib_frame_format_type_progressive;
        ndi_frame.timecode             = to_ndi_timecode(program_pts);
        ndi_frame.p_metadata           = COLOR_METADATA;

        NDIlib_send_send_video_async_v2(sender_, &ndi_frame);
        ++frames_sent_;
    }

    bool is_current_stream(const stream_state_s& state) const
    {
        const std::scoped_lock lock(state_mutex_);
        return worker_running_ && stream_state_.has_value() &&
               stream_state_->configuration_generation == state.configuration_generation;
    }

    static void collect_completed_readbacks(const stream_state_s&                        state,
                                            media::timed_output_queue_s<sender_frame_s>& queue)
    {
        // Always release completed transfer leases into the bounded timing
        // queue. If the sender falls behind, that queue discards obsolete
        // frames here on the output worker instead of letting completed readbacks
        // exhaust the render thread's preallocated slots.
        while (true) {
            auto readback = state.stream->try_consume_oldest();
            if (!readback.has_value()) {
                break;
            }
            const auto expected_size =
                static_cast<size_t>(state.dimensions.x) * static_cast<size_t>(state.dimensions.y) * 4;
            if (readback->readable_host_bytes().size() != expected_size) {
                continue;
            }
            queue.push({
                .program_target_time = readback->program_target_time(),
                .payload =
                    {
                              .readback   = std::make_shared<gpu::transfer::texture_readback_frame_s>(std::move(*readback)),
                              .dimensions = state.dimensions,
                              },
            });
        }
    }

    void run_stream(const stream_state_s& state)
    {
        media::timed_output_queue_s<sender_frame_s>              queue({
                         .capacity        = output_sender_s::get_queue_capacity(state.buffer_frames),
                         .early_tolerance = state.frame_duration / 2,
        });
        media::presentation_timeline_s                           timeline;
        utils::flicks                                            output_deadline{};
        std::shared_ptr<gpu::transfer::texture_readback_frame_s> inflight;
        bool                                                     started{};

        while (true) {
            if (!is_current_stream(state)) {
                break;
            }

            collect_completed_readbacks(state, queue);
            publish_queue_metrics(queue);

            if (!started && queue.queued() < state.buffer_frames) {
                std::unique_lock lock(state_mutex_);
                state_condition_.wait_for(lock, steady_duration(state.frame_duration));
                continue;
            }

            const auto now = utils::flicks_now();
            if (!started) {
                const auto oldest_program_target_time = queue.oldest_program_target_time();
                if (!oldest_program_target_time.has_value()) {
                    phase_ = phase_e::failed;
                    break;
                }
                output_deadline           = now;
                const auto output_latency = timeline.observe_latency(output_deadline, *oldest_program_target_time);
                output_latency_us_ = std::chrono::duration_cast<std::chrono::microseconds>(output_latency).count();
                started            = true;
            }
            if (now < output_deadline) {
                std::unique_lock lock(state_mutex_);
                state_condition_.wait_until(lock, steady_time_point(output_deadline));
                continue;
            }

            if (state.frame_duration <= utils::flicks::zero()) {
                phase_ = phase_e::failed;
                break;
            }
            const auto obsolete_intervals = static_cast<uint64_t>((now - output_deadline) / state.frame_duration);
            if (obsolete_intervals != 0) {
                output_deadline += state.frame_duration * static_cast<utils::flicks::rep>(obsolete_intervals);
                output_intervals_skipped_.fetch_add(obsolete_intervals);
            }

            const auto program_target = timeline.map_presentation_to_program_target(output_deadline);
            if (!program_target.has_value()) {
                phase_ = phase_e::failed;
                break;
            }
            const auto selection = queue.select(*program_target);
            publish_queue_metrics(queue);
            if (selection.frame != nullptr) {
                if (selection.selection == media::output_frame_selection_e::new_frame) {
                    output_latency_us_ =
                        std::chrono::duration_cast<std::chrono::microseconds>(
                            timeline.observe_latency(output_deadline, selection.frame->program_target_time))
                            .count();
                }
                program_selection_offset_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                                                   selection.frame->program_target_time - *program_target)
                                                   .count();
                auto next = selection.frame->payload.readback;
                send_frame(state, selection.frame->payload, *program_target - state.program_time_origin);
                inflight = std::move(next);
            }

            output_deadline += state.frame_duration;
        }

        NDIlib_send_send_video_async_v2(sender_, nullptr);
        inflight.reset();
    }

    void worker_loop()
    {
        while (true) {
            stream_state_s state;
            {
                std::unique_lock lock(state_mutex_);
                state_condition_.wait(lock, [this] { return !worker_running_ || stream_state_.has_value(); });
                if (!worker_running_) {
                    return;
                }
                if (!stream_state_.has_value()) {
                    continue;
                }
                state = *stream_state_;
            }
            run_stream(state);
        }
    }

  public:
    impl_s(utils::serial_executor_s* control_executor, std::string sender_name)
        : control_executor_(control_executor)
        , sender_name_(std::move(sender_name))
    {
    }

    impl_s(const impl_s&)            = delete;
    impl_s(impl_s&&)                 = delete;
    impl_s& operator=(const impl_s&) = delete;
    impl_s& operator=(impl_s&&)      = delete;

    ~impl_s() { stop_control(); }

    void post_control(std::shared_ptr<output_sender_s> owner, void (impl_s::*task)())
    {
        if (!control_executor_->post([owner = std::move(owner), task] { (owner->impl_.get()->*task)(); })) {
            phase_ = phase_e::failed;
        }
    }

    void start_control()
    {
        if (stop_requested_.load()) {
            phase_ = phase_e::stopped;
            return;
        }

        NDIlib_send_create_t create{};
        create.p_ndi_name  = sender_name_.c_str();
        create.clock_video = false;
        create.clock_audio = false;

        sender_ = NDIlib_send_create(&create);
        if (sender_ == nullptr) {
            log()->error("NDIlib_send_create failed for \"{}\"", sender_name_);
            phase_ = phase_e::failed;
            return;
        }

        {
            const std::scoped_lock lock(state_mutex_);
            worker_running_ = true;
        }
        worker_ = std::thread(&impl_s::worker_loop, this);
        phase_  = phase_e::running;
        log()->info("NDI sender running: \"{}\"", sender_name_);
    }

    void stop_control()
    {
        const auto previous = phase_.exchange(phase_e::stopping);
        if (previous == phase_e::stopped) {
            phase_ = phase_e::stopped;
            return;
        }

        {
            const std::scoped_lock lock(state_mutex_);
            worker_running_ = false;
            stream_state_.reset();
        }
        state_condition_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }

        if (sender_ != nullptr) {
            NDIlib_send_destroy(sender_);
            sender_ = nullptr;
        }
        phase_ = phase_e::stopped;
    }

    void request_stop() { stop_requested_ = true; }

    phase_e phase() const { return phase_.load(); }

    output_sender_s::metrics_s metrics() const
    {
        return {
            .program_frames_received      = program_frames_received_.load(),
            .program_queue_overflow_drops = program_queue_overflow_drops_.load(),
            .program_timing_drops         = program_timing_drops_.load(),
            .program_frames_repeated      = program_frames_repeated_.load(),
            .program_frames_missing       = program_frames_missing_.load(),
            .output_intervals_skipped     = output_intervals_skipped_.load(),
            .frames_sent                  = frames_sent_.load(),
            .queued_frames                = queued_frames_.load(),
            .output_latency_us            = output_latency_us_.load(),
            .program_selection_offset_us  = program_selection_offset_us_.load(),
        };
    }

    void set_stream(std::shared_ptr<gpu::transfer::texture_readback_stream_s> stream,
                    gpu::vec2i_t                                              dimensions,
                    frame_rate_s                                              frame_rate,
                    utils::flicks                                             frame_duration,
                    utils::flicks                                             program_time_origin,
                    size_t                                                    buffer_frames)
    {
        output_sender_s::validate_buffer_frame_count(buffer_frames);
        {
            const std::scoped_lock lock(state_mutex_);
            stream_state_ = stream_state_s{
                .stream                   = std::move(stream),
                .dimensions               = dimensions,
                .frame_rate               = frame_rate,
                .frame_duration           = frame_duration,
                .program_time_origin      = program_time_origin,
                .buffer_frames            = buffer_frames,
                .configuration_generation = ++stream_configuration_generation_,
            };
        }
        state_condition_.notify_all();
    }

    void clear_stream()
    {
        {
            const std::scoped_lock lock(state_mutex_);
            stream_state_.reset();
            ++stream_configuration_generation_;
        }
        state_condition_.notify_all();
    }

    void notify_frame() { state_condition_.notify_one(); }
};

output_sender_s::output_sender_s(utils::serial_executor_s* control_executor, std::string sender_name)
    : impl_(std::make_unique<impl_s>(control_executor, std::move(sender_name)))
{
}

size_t output_sender_s::validate_buffer_frame_count(size_t buffer_frames)
{
    if (std::cmp_less(buffer_frames, ndi_output_buffer_limits_s::MINIMUM_FRAME_COUNT) ||
        std::cmp_greater(buffer_frames, ndi_output_buffer_limits_s::MAXIMUM_FRAME_COUNT)) {
        throw std::invalid_argument("NDI output buffer frame count is outside its supported range");
    }
    return buffer_frames;
}

size_t output_sender_s::get_queue_capacity(size_t buffer_frames)
{
    return validate_buffer_frame_count(buffer_frames) + 3;
}

size_t output_sender_s::get_readback_slot_count(size_t buffer_frames)
{
    // The timing queue retains its current frame separately from queued(),
    // while the pipeline headroom covers two consecutive render evaluations
    // permitted by the current one-frame-late policy.
    return get_queue_capacity(buffer_frames) + RETAINED_PROGRAM_FRAME_COUNT + READBACK_PIPELINE_HEADROOM;
}

std::shared_ptr<output_sender_s> output_sender_s::create(utils::serial_executor_s* control_executor,
                                                         std::string               sender_name)
{
    return std::shared_ptr<output_sender_s>(new output_sender_s(control_executor, std::move(sender_name)));
}

output_sender_s::~output_sender_s() = default;

void output_sender_s::start_async() { impl_->post_control(shared_from_this(), &impl_s::start_control); }

void output_sender_s::stop_async()
{
    impl_->request_stop();
    impl_->post_control(shared_from_this(), &impl_s::stop_control);
}

output_sender_s::phase_e output_sender_s::phase() const { return impl_->phase(); }

output_sender_s::metrics_s output_sender_s::metrics() const { return impl_->metrics(); }

void output_sender_s::set_stream(std::shared_ptr<gpu::transfer::texture_readback_stream_s> stream,
                                 gpu::vec2i_t                                              dimensions,
                                 frame_rate_s                                              frame_rate,
                                 utils::flicks                                             frame_duration,
                                 utils::flicks                                             program_time_origin,
                                 size_t                                                    buffer_frames)
{
    impl_->set_stream(std::move(stream), dimensions, frame_rate, frame_duration, program_time_origin, buffer_frames);
}

void output_sender_s::clear_stream() { impl_->clear_stream(); }

void output_sender_s::notify_frame() { impl_->notify_frame(); }

} // namespace miximus::nodes::ndi::detail
