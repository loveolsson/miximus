#include "output_sender.hpp"

#include "gpu/transfer/texture_download.hpp"
#include "logger/logger.hpp"
#include "media/timed_output_queue.hpp"
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
#include <thread>
#include <utility>

namespace miximus::nodes::ndi::detail {
namespace {
auto log() { return getlog("ndi"); }

constexpr auto COLOR_METADATA = R"(<ndi_color_info primaries="bt_709" transfer="bt_709" matrix="bt_709"/>)";

struct sender_frame_s
{
    std::shared_ptr<gpu::transfer::texture_download_frame_s> download;
    gpu::vec2i_t                                             dimensions;
};
} // namespace

class output_sender_s::impl_s
{
    struct stream_state_s
    {
        std::shared_ptr<gpu::transfer::texture_download_stream_s> stream;
        gpu::vec2i_t                                              dimensions;
        frame_rate_s                                              frame_rate;
        uint64_t                                                  epoch{};
        utils::flicks                                             frame_duration;
        size_t                                                    buffer_frames{};
        uint64_t                                                  generation{};
    };

    utils::serial_executor_s* control_executor_;
    std::string               sender_name_;
    NDIlib_send_instance_t    sender_{nullptr};

    std::atomic<phase_e> phase_{phase_e::starting};
    std::atomic_bool     stop_requested_{};

    mutable std::mutex            state_mutex_;
    std::condition_variable       state_condition_;
    std::optional<stream_state_s> stream_state_;
    uint64_t                      stream_generation_{};
    bool                          worker_running_{};
    std::thread                   worker_;

    std::atomic_uint64_t program_frames_received_{};
    std::atomic_uint64_t program_queue_overflow_drops_{};
    std::atomic_uint64_t program_timing_drops_{};
    std::atomic_uint64_t program_frames_repeated_{};
    std::atomic_uint64_t program_frames_missing_{};
    std::atomic_uint64_t output_intervals_skipped_{};
    std::atomic_uint64_t frames_sent_{};
    std::atomic_size_t   queued_frames_{};

    static int64_t to_ndi_timecode(utils::flicks pts)
    {
        constexpr int64_t NDI_TICKS_PER_SECOND = 10'000'000;
        const auto        seconds              = pts.count() / utils::k_flicks_one_second.count();
        const auto        remainder            = pts.count() % utils::k_flicks_one_second.count();
        return seconds * NDI_TICKS_PER_SECOND + remainder * NDI_TICKS_PER_SECOND / utils::k_flicks_one_second.count();
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

    void send_frame(const stream_state_s& state, const sender_frame_s& frame, utils::flicks output_pts)
    {
        const auto bytes = frame.download->bytes();

        NDIlib_video_frame_v2_t ndi_frame{};
        ndi_frame.xres                 = frame.dimensions.x;
        ndi_frame.yres                 = frame.dimensions.y;
        ndi_frame.FourCC               = NDIlib_FourCC_video_type_RGBA;
        ndi_frame.line_stride_in_bytes = frame.dimensions.x * 4;
        ndi_frame.p_data               = ndi_sdk::send_buffer(bytes);
        ndi_frame.frame_rate_N         = static_cast<int>(state.frame_rate.numerator);
        ndi_frame.frame_rate_D         = static_cast<int>(state.frame_rate.denominator);
        ndi_frame.frame_format_type    = NDIlib_frame_format_type_progressive;
        ndi_frame.timecode             = to_ndi_timecode(output_pts);
        ndi_frame.p_metadata           = COLOR_METADATA;

        NDIlib_send_send_video_async_v2(sender_, &ndi_frame);
        ++frames_sent_;
    }

    void run_stream(stream_state_s state)
    {
        media::timed_output_queue_s<sender_frame_s>              queue({
                         .capacity        = output_sender_s::get_queue_capacity(state.buffer_frames),
                         .early_tolerance = state.frame_duration / 2,
        });
        std::optional<utils::flicks>                             output_pts;
        std::optional<std::chrono::steady_clock::time_point>     output_deadline;
        std::shared_ptr<gpu::transfer::texture_download_frame_s> inflight;
        bool                                                     started{};

        while (true) {
            {
                const std::scoped_lock lock(state_mutex_);
                if (!worker_running_ || !stream_state_.has_value() || stream_state_->generation != state.generation) {
                    break;
                }
            }

            while (auto download = state.stream->try_consume_oldest()) {
                if (std::cmp_greater(download->tag(), std::numeric_limits<utils::flicks::rep>::max())) {
                    continue;
                }
                const auto expected_size =
                    static_cast<size_t>(state.dimensions.x) * static_cast<size_t>(state.dimensions.y) * 4;
                if (download->bytes().size() != expected_size) {
                    continue;
                }
                const auto pts = utils::flicks(static_cast<utils::flicks::rep>(download->tag()));
                if (!output_pts.has_value()) {
                    output_pts = pts;
                }
                queue.push({
                    .id =
                        {
                             .epoch    = state.epoch,
                             .sequence = download->tag(),
                             .pts      = pts,
                             .duration = state.frame_duration,
                             },
                    .value =
                        {
                             .download = std::make_shared<gpu::transfer::texture_download_frame_s>(std::move(*download)),
                             .dimensions = state.dimensions,
                             },
                });
            }
            publish_queue_metrics(queue);

            if (!output_pts.has_value() || (!started && queue.queued() < state.buffer_frames)) {
                std::unique_lock lock(state_mutex_);
                state_condition_.wait_for(lock, steady_duration(state.frame_duration));
                continue;
            }
            started = true;

            const auto now = std::chrono::steady_clock::now();
            if (!output_deadline.has_value()) {
                output_deadline = now;
            }
            if (now < *output_deadline) {
                std::unique_lock lock(state_mutex_);
                state_condition_.wait_until(lock, *output_deadline);
                continue;
            }

            const auto duration = steady_duration(state.frame_duration);
            if (duration <= std::chrono::steady_clock::duration::zero()) {
                phase_ = phase_e::failed;
                break;
            }
            const auto obsolete_intervals = static_cast<uint64_t>((now - *output_deadline) / duration);
            if (obsolete_intervals != 0) {
                *output_pts += state.frame_duration * static_cast<utils::flicks::rep>(obsolete_intervals);
                *output_deadline += duration * obsolete_intervals;
                output_intervals_skipped_.fetch_add(obsolete_intervals);
            }

            const auto selection = queue.select(state.epoch, *output_pts);
            publish_queue_metrics(queue);
            if (selection.frame != nullptr) {
                auto next = selection.frame->value.download;
                send_frame(state, selection.frame->value, *output_pts);
                inflight = std::move(next);
            }

            *output_pts += state.frame_duration;
            *output_deadline += duration;
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
                state = *stream_state_;
            }
            run_stream(std::move(state));
        }
    }

  public:
    impl_s(utils::serial_executor_s* control_executor, std::string sender_name)
        : control_executor_(control_executor)
        , sender_name_(std::move(sender_name))
    {
    }

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
        };
    }

    void set_stream(std::shared_ptr<gpu::transfer::texture_download_stream_s> stream,
                    gpu::vec2i_t                                              dimensions,
                    frame_rate_s                                              frame_rate,
                    uint64_t                                                  epoch,
                    utils::flicks                                             frame_duration,
                    size_t                                                    buffer_frames)
    {
        {
            const std::scoped_lock lock(state_mutex_);
            stream_state_ = stream_state_s{
                .stream         = std::move(stream),
                .dimensions     = dimensions,
                .frame_rate     = frame_rate,
                .epoch          = epoch,
                .frame_duration = frame_duration,
                .buffer_frames  = buffer_frames,
                .generation     = ++stream_generation_,
            };
        }
        state_condition_.notify_all();
    }

    void clear_stream()
    {
        {
            const std::scoped_lock lock(state_mutex_);
            stream_state_.reset();
            ++stream_generation_;
        }
        state_condition_.notify_all();
    }

    void notify_frame() { state_condition_.notify_one(); }
};

output_sender_s::output_sender_s(utils::serial_executor_s* control_executor, std::string sender_name)
    : impl_(std::make_unique<impl_s>(control_executor, std::move(sender_name)))
{
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

void output_sender_s::set_stream(std::shared_ptr<gpu::transfer::texture_download_stream_s> stream,
                                 gpu::vec2i_t                                              dimensions,
                                 frame_rate_s                                              frame_rate,
                                 uint64_t                                                  epoch,
                                 utils::flicks                                             frame_duration,
                                 size_t                                                    buffer_frames)
{
    impl_->set_stream(std::move(stream), dimensions, frame_rate, epoch, frame_duration, buffer_frames);
}

void output_sender_s::clear_stream() { impl_->clear_stream(); }

void output_sender_s::notify_frame() { impl_->notify_frame(); }

} // namespace miximus::nodes::ndi::detail
