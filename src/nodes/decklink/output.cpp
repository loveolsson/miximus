#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/colorspace.hpp"
#include "detail/device_reservation.hpp"
#include "detail/output_activation.hpp"
#include "detail/output_path.hpp"
#include "detail/output_video_buffer.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_readback.hpp"
#include "logger/logger.hpp"
#include "media/frame_fingerprint.hpp"
#include "media/media_clock.hpp"
#include "media/output_runtime_metrics.hpp"
#include "media/presentation_timeline.hpp"
#include "media/timed_output_queue.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
#include "types/node_status_json.hpp"
#include "types/output_buffer_limits.hpp"
#include "types/settings_option.hpp"
#include "utils/lookup.hpp"
#include "utils/observed_value.hpp"
#include "utils/serial_executor.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"
#include "wrapper/decklink-sdk/platform_compat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace miximus;
using namespace miximus::decklink_sdk;
using namespace miximus::nodes;
using namespace miximus::nodes::decklink;
using namespace miximus::nodes::decklink::detail;
using namespace std::chrono_literals;

constexpr size_t OUTPUT_QUEUE_HEADROOM        = 3;
constexpr size_t READBACK_PIPELINE_HEADROOM   = 3;
constexpr size_t RETAINED_PROGRAM_FRAME_COUNT = 1;
constexpr size_t PROGRAM_QUEUE_RESERVE        = 1;

constexpr size_t get_output_queue_capacity(size_t buffer_frames) { return buffer_frames + OUTPUT_QUEUE_HEADROOM; }

constexpr size_t get_readback_slot_count(size_t scheduled_frames, size_t program_queue_capacity)
{
    // The timed queue retains its current frame separately from queued(), so
    // it needs its own slot in addition to scheduled frames, queued program
    // frames, and the render/readback pipeline.
    return scheduled_frames + program_queue_capacity + RETAINED_PROGRAM_FRAME_COUNT + READBACK_PIPELINE_HEADROOM;
}

auto log() { return getlog("decklink"); }

status::decklink_output_device_status_s make_device_status(const device_status_s& status)
{
    return {
        .reference_locked    = status.reference_signal_locked,
        .playback_busy       = status.playback_busy,
        .pcie_link_width     = status.pcie_link_width,
        .pcie_link_speed     = status.pcie_link_speed,
        .temperature_c       = status.temperature_c,
        .active_format       = status.current_output_mode,
        .output_pixel_format = status.last_output_pixel_format,
        .reference_format    = status.reference_signal_mode,
    };
}

class callback_s final : public IDeckLinkVideoOutputCallback
{
  public:
    enum class phase_e : uint8_t
    {
        starting,
        prerolling,
        running,
        stopping,
        stopped,
        failed,
    };

    struct metrics_s
    {
        uint64_t                                         frames_completed{};
        uint64_t                                         frames_displayed_late{};
        uint64_t                                         frames_dropped{};
        uint64_t                                         frames_flushed{};
        uint64_t                                         program_frames_received{};
        uint64_t                                         program_queue_overflow_drops{};
        uint64_t                                         program_timing_drops{};
        uint64_t                                         program_frames_repeated{};
        uint64_t                                         program_frames_missing{};
        uint64_t                                         program_cadence_repeats{};
        uint64_t                                         program_starvation_repeats{};
        uint64_t                                         program_starvation_repeat_streak{};
        uint64_t                                         program_starvation_repeat_streak_max{};
        uint64_t                                         output_refill_shortfalls{};
        uint64_t                                         content_frames_sampled{};
        uint64_t                                         content_frame_repeats{};
        uint64_t                                         content_repeat_streak{};
        uint64_t                                         content_repeat_streak_max{};
        uint64_t                                         completion_intervals{};
        int64_t                                          completion_interval_max_us{};
        size_t                                           program_queue_depth{};
        size_t                                           program_queue_depth_max{};
        uint32_t                                         buffered_video_frames{};
        uint32_t                                         buffered_video_frames_min{};
        uint32_t                                         buffered_video_frames_max{};
        uint64_t                                         buffered_below_target_samples{};
        uint64_t                                         buffered_zero_samples{};
        int64_t                                          output_latency_us{};
        int64_t                                          program_selection_offset_us{};
        uint64_t                                         completion_time_failures{};
        gpu::transfer::texture_readback_stream_metrics_s readback_stream;
    };

    struct render_state_s
    {
        std::shared_ptr<gpu::transfer::texture_readback_stream_s> readback_stream;
        active_output_s                                           active_output;
    };

  private:
    using reservation_s = device_reservation_s<IDeckLinkOutput>;

    struct scheduled_frame_s
    {
        decklink_ptr<IDeckLinkVideoFrame> frame;
        uint64_t                          output_frame_sequence{};
        utils::flicks                     decklink_media_pts{};
        utils::flicks                     program_target_time{};
    };

    std::atomic_ulong ref_count_{1};

    gpu::transfer::texture_readback_service_s* readback_service_;
    utils::serial_executor_s*                  control_executor_;
    decklink_ptr<IDeckLinkOutput>              device_;
    std::shared_ptr<reservation_s>             reservation_;
    std::string                                device_name_;
    std::string                                requested_mode_name_;
    keyer_mode_e                               requested_keyer_mode_;
    output_activation_s                        output_activation_;

    mutable std::mutex             state_mutex_;
    std::optional<render_state_s>  render_state_;
    std::vector<settings_option_s> mode_options_;
    std::atomic_uint64_t           mode_options_version_;

    mutable std::mutex                                                             frame_mutex_;
    std::shared_ptr<gpu::transfer::texture_readback_stream_s>                      readback_stream_;
    decklink_ptr<IDeckLinkVideoBuffer>                                             last_buffer_;
    std::optional<media::timed_output_queue_s<decklink_ptr<IDeckLinkVideoBuffer>>> output_queue_;
    media::output_runtime_metrics_s                                                runtime_metrics_;
    std::deque<scheduled_frame_s>                                                  scheduled_frames_;
    active_output_s                                                                active_output_;
    BMDTimeValue                                                                   next_decklink_stream_time_{};
    uint64_t                                                                       next_output_frame_sequence_{};
    utils::flicks                                                                  program_frame_duration_;
    media::media_to_program_clock_s                                                decklink_to_program_clock_;
    media::presentation_timeline_s                                                 presentation_timeline_;
    size_t                                                                         configured_buffer_frames_{};
    size_t                                                                         scheduled_frame_target_{};
    uint64_t                                                                       content_frames_sampled_{};
    uint64_t                                                                       content_frame_repeats_{};
    uint64_t                                                                       content_repeat_streak_{};
    uint64_t                                                                       content_repeat_streak_max_{};
    uint64_t                                                                       last_content_fingerprint_{};
    int64_t                                                                        program_selection_offset_us_{};
    uint64_t                                                                       completion_time_failures_{};

    std::atomic_bool     stop_requested_;
    std::atomic_bool     preroll_pump_posted_;
    std::atomic<phase_e> phase_{phase_e::starting};
    bool                 callback_installed_{};
    bool                 playback_started_{};

    std::mutex              stop_mutex_;
    std::condition_variable stop_condition_;
    bool                    playback_stopped_{};

    std::atomic_uint64_t frames_completed_;
    std::atomic_uint64_t frames_displayed_late_;
    std::atomic_uint64_t frames_dropped_;
    std::atomic_uint64_t frames_flushed_;

    void post_control(std::function<void(callback_s&)> task)
    {
        decklink_ptr<callback_s> self(this);
        const bool accepted = control_executor_->post([self = std::move(self), task = std::move(task)]() mutable {
            try {
                task(*self);
            } catch (const std::exception& error) {
                log()->error("DeckLink output control task failed: {}", error.what());
                self->stop_requested_ = true;
                self->retire_playback(phase_e::failed);
            } catch (...) {
                log()->error("DeckLink output control task failed");
                self->stop_requested_ = true;
                self->retire_playback(phase_e::failed);
            }
        });
        if (!accepted) {
            log()->error("DeckLink control executor rejected an output task during shutdown");
            phase_ = phase_e::failed;
        }
    }

    void publish_mode_options(std::vector<settings_option_s> options)
    {
        {
            const std::scoped_lock lock(state_mutex_);
            mode_options_ = std::move(options);
        }
        mode_options_version_.fetch_add(1);
    }

    auto select_display_mode() -> std::optional<output_display_mode_s>
    {
        decklink_ptr<IDeckLinkDisplayModeIterator> iterator;
        if (!device_ || device_->GetDisplayModeIterator(iterator.releaseAndGetAddressOf()) != S_OK || !iterator) {
            log()->error("Failed to enumerate display modes for DeckLink output {}", device_name_);
            return std::nullopt;
        }

        std::optional<output_display_mode_s> selected;
        std::vector<std::string>             names;
        decklink_ptr<IDeckLinkDisplayMode>   sdk_mode;
        while (iterator->Next(sdk_mode.releaseAndGetAddressOf()) == S_OK) {
            output_display_mode_s mode{};
            mode.mode       = sdk_mode->GetDisplayMode();
            mode.dimensions = {sdk_mode->GetWidth(), sdk_mode->GetHeight()};
            if (sdk_mode->GetFrameRate(&mode.frame_duration, &mode.time_scale) != S_OK) {
                continue;
            }
            if (mode.frame_duration <= 0 || mode.time_scale <= 0 ||
                std::cmp_greater(mode.frame_duration, std::numeric_limits<uint32_t>::max()) ||
                std::cmp_greater(mode.time_scale, std::numeric_limits<uint32_t>::max())) {
                continue;
            }
            const auto frame_duration = get_frame_duration({
                .numerator   = static_cast<uint32_t>(mode.time_scale),
                .denominator = static_cast<uint32_t>(mode.frame_duration),
            });
            if (!frame_duration.has_value()) {
                continue;
            }
            mode.frame_duration_flicks = *frame_duration;

            mode.colorspace       = get_display_mode_colorspace(sdk_mode.get());
            const auto transfer   = get_color_transfer(mode.colorspace);
            mode.yuv_conversion   = gpu::get_color_transfer_to_yuv(transfer);
            mode.gamut_conversion = gpu::get_gamut_transfer_from_rec709(transfer);

            auto name = get_display_mode_name(sdk_mode.get());
            if (name == requested_mode_name_) {
                selected = mode;
            }
            names.emplace_back(std::move(name));
        }

        publish_mode_options(make_settings_options_with_matching_labels(names));
        if (!selected) {
            log()->error("DeckLink output mode {} was not found on {}", requested_mode_name_, device_name_);
        }
        return selected;
    }

    size_t minimum_preroll_frames()
    {
        auto    attributes = device_.query<IDeckLinkProfileAttributes>();
        int64_t minimum{};
        if (!attributes || attributes->GetInt(BMDDeckLinkMinimumPrerollFrames, &minimum) != S_OK || minimum <= 0) {
            log()->warn("Unable to query minimum preroll for {}; using the configured buffer depth", device_name_);
            return 1;
        }
        return static_cast<size_t>(minimum);
    }

    auto create_frame(IDeckLinkVideoBuffer* buffer) -> decklink_ptr<IDeckLinkVideoFrame>
    {
        return active_output_.path->create_frame(device_.get(), buffer, device_name_);
    }

    bool initialize_output()
    {
        const auto selected_mode = select_display_mode();
        if (!selected_mode) {
            return false;
        }
        const auto active_output = output_activation_.start(*selected_mode, requested_keyer_mode_);
        if (!active_output) {
            return false;
        }

        const auto device_minimum = minimum_preroll_frames();
        if (configured_buffer_frames_ < device_minimum) {
            log()->info("DeckLink output {} requires at least {} buffered frames; configured {}",
                        device_name_,
                        device_minimum,
                        configured_buffer_frames_);
        }
        const auto buffer_frames = std::max(configured_buffer_frames_, device_minimum);
        scheduled_frame_target_  = buffer_frames;
        output_queue_.emplace(media::timed_output_queue_config_s{
            .capacity        = get_output_queue_capacity(buffer_frames),
            .early_tolerance = program_frame_duration_ / 2,
        });

        const auto readback_slot_count = get_readback_slot_count(scheduled_frame_target_, output_queue_->capacity());
        const auto stream              = readback_service_->create_stream({
                         .transfer_layout = active_output->path->transfer_layout(),
                         .max_slots       = readback_slot_count,
                         .initial_slots   = readback_slot_count,
        });
        if (!stream->wait_for_initial_slots(5s)) {
            log()->error("Failed to initialize the DeckLink output transfer pool for {}", device_name_);
            return false;
        }

        active_output_   = *active_output;
        readback_stream_ = stream;
        {
            const std::scoped_lock lock(state_mutex_);
            render_state_.emplace(render_state_s{.readback_stream = stream, .active_output = *active_output});
        }

        if (device_->SetScheduledFrameCompletionCallback(this) != S_OK) {
            log()->error("Failed to install DeckLink output callback for {}", device_name_);
            return false;
        }
        callback_installed_ = true;
        return true;
    }

    void retire_playback(phase_e final_phase)
    {
        phase_ = phase_e::stopping;

        if (device_ && playback_started_) {
            const auto stopped = device_->StopScheduledPlayback(0, nullptr, 0);
            if (stopped == S_OK) {
                std::unique_lock lock(stop_mutex_);
                if (!stop_condition_.wait_for(lock, 2s, [this] { return playback_stopped_; })) {
                    log()->warn("Timed out waiting for DeckLink scheduled playback to stop: {}", device_name_);
                }
            } else {
                log()->warn("Failed to request DeckLink scheduled playback stop: {}", device_name_);
            }
            playback_started_ = false;
        }

        if (device_ && callback_installed_) {
            (void)device_->SetScheduledFrameCompletionCallback(nullptr);
            callback_installed_ = false;
        }
        output_activation_.stop();

        {
            const std::scoped_lock lock(frame_mutex_);
            last_buffer_ = nullptr;
            output_queue_.reset();
            scheduled_frames_.clear();
            readback_stream_.reset();
        }
        {
            const std::scoped_lock lock(state_mutex_);
            render_state_.reset();
        }

        device_ = nullptr;
        reservation_.reset();
        phase_ = final_phase;
    }

    void start_control()
    {
        if (stop_requested_.load()) {
            retire_playback(phase_e::stopped);
            return;
        }
        if (!initialize_output()) {
            stop_requested_ = true;
            retire_playback(phase_e::failed);
            return;
        }
        if (stop_requested_.load()) {
            retire_playback(phase_e::stopped);
            return;
        }

        phase_ = phase_e::prerolling;
        log()->info("DeckLink output ready for program-frame preroll: {}", device_name_);
    }

    void request_failure()
    {
        if (!stop_requested_.exchange(true)) {
            phase_ = phase_e::stopping;
            post_control([](callback_s& self) { self.retire_playback(phase_e::failed); });
        }
    }

    void sample_buffered_video_frames()
    {
        uint32_t buffered{};
        if (device_->GetBufferedVideoFrameCount(&buffered) == S_OK) {
            runtime_metrics_.observe_buffered_frames(buffered, static_cast<uint32_t>(scheduled_frame_target_));
        }
    }

    void collect_completed_readbacks()
    {
        if (!output_queue_.has_value()) {
            return;
        }
        auto& output_queue = *output_queue_;

        // The timed queue owns its overflow policy. Drain every completed
        // lease so obsolete queued frames are released instead of exhausting
        // the readback stream.
        while (true) {
            auto frame = readback_stream_->try_consume_oldest();
            if (!frame.has_value()) {
                break;
            }
            if (frame->readable_host_bytes().size() != active_output_.path->transfer_layout().host_buffer_size_bytes) {
                log()->error("DeckLink output transfer produced an unexpected buffer size");
                continue;
            }
            const auto program_target_time = frame->program_target_time();
            const auto fingerprint         = media::sampled_frame_fingerprint(frame->readable_host_bytes());
            if (content_frames_sampled_ != 0 && fingerprint == last_content_fingerprint_) {
                ++content_frame_repeats_;
                ++content_repeat_streak_;
                content_repeat_streak_max_ = std::max(content_repeat_streak_max_, content_repeat_streak_);
            } else {
                content_repeat_streak_ = 0;
            }
            ++content_frames_sampled_;
            last_content_fingerprint_ = fingerprint;
            auto buffer               = make_decklink_ptr<output_video_buffer_s>(std::move(*frame));
            output_queue.push({
                .program_target_time = program_target_time,
                .payload             = buffer.query<IDeckLinkVideoBuffer>(),
            });
        }
    }

    bool schedule_program_frame(utils::flicks program_target_time)
    {
        if (!output_queue_.has_value()) {
            return false;
        }
        auto&      output_queue = *output_queue_;
        const auto selection    = output_queue.select(program_target_time);
        runtime_metrics_.observe_selection(selection.selection, output_queue.queued() != 0);
        if (selection.frame != nullptr) {
            last_buffer_                 = selection.frame->payload;
            program_selection_offset_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
                                               selection.frame->program_target_time - program_target_time)
                                               .count();
        }
        if (!last_buffer_) {
            return false;
        }

        auto       frame              = create_frame(last_buffer_.get());
        const auto stream_time        = next_decklink_stream_time_;
        const auto decklink_media_pts = active_output_.path->display_mode().frame_duration_flicks *
                                        static_cast<utils::flicks::rep>(next_output_frame_sequence_);
        if (!frame) {
            return false;
        }
        const auto schedule_result = device_->ScheduleVideoFrame(frame.get(),
                                                                 stream_time,
                                                                 active_output_.path->display_mode().frame_duration,
                                                                 active_output_.path->display_mode().time_scale);
        if (schedule_result != S_OK) {
            log()->error("ScheduleVideoFrame failed for {} with result {:#010x}",
                         device_name_,
                         static_cast<uint32_t>(schedule_result));
            return false;
        }
        scheduled_frames_.push_back({
            .frame                 = std::move(frame),
            .output_frame_sequence = next_output_frame_sequence_,
            .decklink_media_pts    = decklink_media_pts,
            .program_target_time   = program_target_time,
        });
        next_decklink_stream_time_ += active_output_.path->display_mode().frame_duration;
        ++next_output_frame_sequence_;
        return true;
    }

    auto completed_frame_start_time(IDeckLinkVideoFrame* completed_frame) -> std::optional<utils::flicks>
    {
        BMDTimeValue completion_time{};
        if (completed_frame == nullptr || device_->GetFrameCompletionReferenceTimestamp(
                                              completed_frame, reference_time_scale(), &completion_time) != S_OK) {
            ++completion_time_failures_;
            return std::nullopt;
        }
        const auto steady_before   = utils::flicks_now();
        const auto reference_now   = reference_time_now();
        const auto steady_after    = utils::flicks_now();
        const auto steady_now      = steady_before + (steady_after - steady_before) / 2;
        const auto reference_delta = static_cast<long double>(completion_time - reference_now) /
                                     static_cast<long double>(reference_time_scale());
        return steady_now + utils::to_flicks(static_cast<double>(reference_delta)) -
               active_output_.path->display_mode().frame_duration_flicks;
    }

    void pump_preroll()
    {
        preroll_pump_posted_ = false;
        if (phase_.load() != phase_e::prerolling || stop_requested_.load()) {
            return;
        }

        bool started{};
        {
            const std::scoped_lock lock(frame_mutex_);
            if (!output_queue_.has_value()) {
                request_failure();
                return;
            }
            auto& output_queue = *output_queue_;
            collect_completed_readbacks();
            runtime_metrics_.observe_output_queue_depth(output_queue.queued());
            const auto required_program_frames = scheduled_frame_target_ + PROGRAM_QUEUE_RESERVE;
            if (output_queue.queued() < required_program_frames) {
                return;
            }

            const auto first_program_target = output_queue.oldest_program_target_time();
            if (!first_program_target.has_value()) {
                return;
            }
            auto program_target = *first_program_target;
            for (size_t i = 0; i < scheduled_frame_target_; ++i) {
                if (!schedule_program_frame(program_target)) {
                    break;
                }
                program_target += active_output_.path->display_mode().frame_duration_flicks;
            }
            if (scheduled_frames_.size() != scheduled_frame_target_) {
                runtime_metrics_.observe_refill(scheduled_frame_target_, scheduled_frames_.size());
            } else {
                phase_ = phase_e::running;
                const auto start_result =
                    device_->StartScheduledPlayback(0, active_output_.path->display_mode().time_scale, 1.0);
                if (start_result == S_OK) {
                    playback_started_ = true;
                    sample_buffered_video_frames();
                    started = true;
                } else {
                    log()->error("StartScheduledPlayback failed for {} with result {:#010x}",
                                 device_name_,
                                 static_cast<uint32_t>(start_result));
                    phase_ = phase_e::prerolling;
                }
            }
        }

        if (started) {
            log()->info("DeckLink output playback running after {} program preroll frames: {}",
                        scheduled_frame_target_,
                        device_name_);
        } else {
            log()->error("Failed to start DeckLink output with program-frame preroll: {}", device_name_);
            request_failure();
        }
    }

    HRESULT scheduled_frame_completed(IDeckLinkVideoFrame* completed_frame, bool observe_completion_time)
    {
        const std::scoped_lock lock(frame_mutex_);
        if (phase_.load() != phase_e::running) {
            return S_OK;
        }
        if (!output_queue_.has_value()) {
            request_failure();
            return E_FAIL;
        }
        auto& output_queue = *output_queue_;

        const auto completed =
            std::ranges::find(scheduled_frames_, completed_frame, [](const auto& frame) { return frame.frame.get(); });
        if (completed == scheduled_frames_.end()) {
            log()->error("DeckLink reported an output completion with no scheduled frame");
            request_failure();
            return E_FAIL;
        }

        const auto completed_output_frame_sequence = completed->output_frame_sequence;
        const auto completed_decklink_media_pts    = completed->decklink_media_pts;
        const auto completed_program_target_time   = completed->program_target_time;
        const auto completed_start_time =
            observe_completion_time ? completed_frame_start_time(completed_frame) : std::nullopt;
        scheduled_frames_.erase(completed);
        if (completed_start_time.has_value()) {
            runtime_metrics_.observe_completion(*completed_start_time);
            decklink_to_program_clock_.observe(
                {
                    .stream_epoch   = 1,
                    .frame_sequence = completed_output_frame_sequence,
                    .media_pts      = completed_decklink_media_pts,
                    .frame_duration = active_output_.path->display_mode().frame_duration_flicks,
                },
                *completed_start_time);
            presentation_timeline_.observe_latency(*completed_start_time, completed_program_target_time);
        }
        sample_buffered_video_frames();
        collect_completed_readbacks();
        const auto next_decklink_media_pts = active_output_.path->display_mode().frame_duration_flicks *
                                             static_cast<utils::flicks::rep>(next_output_frame_sequence_);
        const auto next_presentation_time =
            decklink_to_program_clock_.map_media_pts_to_program_time(next_decklink_media_pts);
        auto program_target = next_presentation_time.has_value()
                                  ? presentation_timeline_.map_presentation_to_program_target(*next_presentation_time)
                                  : std::nullopt;
        if (!program_target.has_value()) {
            log()->error("Failed to query the DeckLink output playhead for {}", device_name_);
            request_failure();
            return E_FAIL;
        }
        const auto oldest_program_target = output_queue.oldest_program_target_time();
        if (oldest_program_target.has_value() &&
            *program_target + program_frame_duration_ / 2 < *oldest_program_target) {
            presentation_timeline_.observe_latency(*next_presentation_time, *oldest_program_target);
            program_target = presentation_timeline_.map_presentation_to_program_target(*next_presentation_time);
        }
        if (!program_target.has_value()) {
            log()->error("Failed to remap the DeckLink output playhead for {}", device_name_);
            request_failure();
            return E_FAIL;
        }
        const auto first_program_target = program_target.value();
        const auto refill_count =
            scheduled_frame_target_ > scheduled_frames_.size() ? scheduled_frame_target_ - scheduled_frames_.size() : 0;
        size_t scheduled{};
        for (size_t i = 0; i < refill_count; ++i) {
            const auto target = first_program_target + active_output_.path->display_mode().frame_duration_flicks *
                                                           static_cast<utils::flicks::rep>(i);
            if (!schedule_program_frame(target)) {
                break;
            }
            ++scheduled;
        }
        runtime_metrics_.observe_refill(refill_count, scheduled);
        runtime_metrics_.observe_output_queue_depth(output_queue.queued());
        if (scheduled != refill_count) {
            request_failure();
            return E_FAIL;
        }
        return S_OK;
    }

  public:
    callback_s(gpu::transfer::texture_readback_service_s* readback_service,
               utils::serial_executor_s*                  control_executor,
               decklink_ptr<IDeckLinkOutput>              device,
               std::shared_ptr<reservation_s>             reservation,
               std::string                                device_name,
               std::string                                requested_mode_name,
               keyer_mode_e                               requested_keyer_mode,
               utils::flicks                              program_frame_duration,
               size_t                                     buffer_frames)
        : readback_service_(readback_service)
        , control_executor_(control_executor)
        , device_(std::move(device))
        , reservation_(std::move(reservation))
        , device_name_(std::move(device_name))
        , requested_mode_name_(std::move(requested_mode_name))
        , requested_keyer_mode_(requested_keyer_mode)
        , output_activation_(device_, device_name_)
        , program_frame_duration_(program_frame_duration)
        , configured_buffer_frames_(buffer_frames)
        , scheduled_frame_target_(buffer_frames)
    {
        if (std::cmp_less(buffer_frames, decklink_output_buffer_limits_s::MINIMUM_FRAME_COUNT) ||
            std::cmp_greater(buffer_frames, decklink_output_buffer_limits_s::MAXIMUM_FRAME_COUNT)) {
            throw std::invalid_argument("DeckLink output buffer frame count is outside its supported range");
        }
    }

    ~callback_s() override = default;

    callback_s(const callback_s&)            = delete;
    callback_s& operator=(const callback_s&) = delete;
    callback_s(callback_s&&)                 = delete;
    callback_s& operator=(callback_s&&)      = delete;

    void start_async()
    {
        post_control([](callback_s& self) { self.start_control(); });
    }

    void request_preroll_pump()
    {
        if (phase_.load() != phase_e::prerolling || preroll_pump_posted_.exchange(true)) {
            return;
        }
        post_control([](callback_s& self) { self.pump_preroll(); });
    }

    void stop_async()
    {
        if (!stop_requested_.exchange(true)) {
            phase_ = phase_e::stopping;
            post_control([](callback_s& self) { self.retire_playback(phase_e::stopped); });
        }
    }

    phase_e phase() const { return phase_.load(); }

    auto render_state() const -> std::optional<render_state_s>
    {
        const std::scoped_lock lock(state_mutex_);
        return render_state_;
    }

    uint64_t mode_options_version() const { return mode_options_version_.load(); }

    auto mode_options() const -> std::vector<settings_option_s>
    {
        const std::scoped_lock lock(state_mutex_);
        return mode_options_;
    }

    auto keyer_status() const -> status::decklink_output_keyer_status_s
    {
        const std::scoped_lock lock(state_mutex_);
        if (render_state_) {
            const auto& active_output = render_state_->active_output;
            return {
                .requested_keyer_mode  = std::string(enum_to_string(active_output.requested_keyer_mode)),
                .active_keyer_mode     = std::string(enum_to_string(active_output.active_keyer_mode)),
                .keyer_fallback_reason = active_output.keyer_fallback_reason,
            };
        }
        return {
            .requested_keyer_mode  = std::string(enum_to_string(requested_keyer_mode_)),
            .active_keyer_mode     = std::string(enum_to_string(keyer_mode_e::disabled)),
            .keyer_fallback_reason = std::nullopt,
        };
    }

    metrics_s metrics() const
    {
        const std::scoped_lock lock(frame_mutex_);
        const auto             output_metrics =
            output_queue_.has_value() ? output_queue_->metrics() : media::timed_output_queue_metrics_s{};
        const auto runtime_metrics = runtime_metrics_.snapshot();
        auto       result          = metrics_s{
                           .frames_completed                     = frames_completed_.load(),
                           .frames_displayed_late                = frames_displayed_late_.load(),
                           .frames_dropped                       = frames_dropped_.load(),
                           .frames_flushed                       = frames_flushed_.load(),
                           .program_frames_received              = output_metrics.pushed,
                           .program_queue_overflow_drops         = output_metrics.overflow_drops,
                           .program_timing_drops                 = output_metrics.selection_drops,
                           .program_frames_repeated              = output_metrics.repeated,
                           .program_frames_missing               = output_metrics.missing,
                           .program_cadence_repeats              = runtime_metrics.cadence_repeats,
                           .program_starvation_repeats           = runtime_metrics.starvation_repeats,
                           .program_starvation_repeat_streak     = runtime_metrics.starvation_repeat_streak,
                           .program_starvation_repeat_streak_max = runtime_metrics.starvation_repeat_streak_max,
                           .output_refill_shortfalls             = runtime_metrics.refill_shortfalls,
                           .content_frames_sampled               = content_frames_sampled_,
                           .content_frame_repeats                = content_frame_repeats_,
                           .content_repeat_streak                = content_repeat_streak_,
                           .content_repeat_streak_max            = content_repeat_streak_max_,
                           .completion_intervals                 = runtime_metrics.completion_intervals,
                           .completion_interval_max_us =
                std::chrono::duration_cast<std::chrono::microseconds>(runtime_metrics.completion_interval_max).count(),
                           .program_queue_depth           = runtime_metrics.output_queue_depth,
                           .program_queue_depth_max       = runtime_metrics.output_queue_depth_max,
                           .buffered_video_frames         = runtime_metrics.buffered_frames,
                           .buffered_video_frames_min     = runtime_metrics.buffered_frames_min,
                           .buffered_video_frames_max     = runtime_metrics.buffered_frames_max,
                           .buffered_below_target_samples = runtime_metrics.buffered_below_target_samples,
                           .buffered_zero_samples         = runtime_metrics.buffered_zero_samples,
                           .output_latency_us =
                presentation_timeline_.latency().has_value()
                                   ? std::chrono::duration_cast<std::chrono::microseconds>(*presentation_timeline_.latency()).count()
                                   : 0,
                           .program_selection_offset_us = program_selection_offset_us_,
                           .completion_time_failures    = completion_time_failures_,
                           .readback_stream             = {},
        };
        if (readback_stream_) {
            result.readback_stream = readback_stream_->metrics();
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame*           completed_frame,
                                                      BMDOutputFrameCompletionResult result) noexcept final
    {
        bool observe_completion_time{};
        switch (result) {
            case bmdOutputFrameCompleted:
                ++frames_completed_;
                observe_completion_time = true;
                break;
            case bmdOutputFrameDisplayedLate:
                ++frames_displayed_late_;
                observe_completion_time = true;
                break;
            case bmdOutputFrameDropped:
                ++frames_dropped_;
                break;
            case bmdOutputFrameFlushed:
                ++frames_flushed_;
                return S_OK;
            default:
                break;
        }

        if (phase_.load() != phase_e::running) {
            return S_OK;
        }
        try {
            return scheduled_frame_completed(completed_frame, observe_completion_time);
        } catch (const std::exception& error) {
            logger::log_error_noexcept("decklink", "DeckLink output callback failed: {}", error.what());
        } catch (...) {
            logger::log_error_noexcept("decklink", "DeckLink output callback failed");
        }
        request_failure();
        return E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() noexcept final
    {
        try {
            {
                const std::scoped_lock lock(stop_mutex_);
                playback_stopped_ = true;
            }
            stop_condition_.notify_all();
            return S_OK;
        } catch (...) {
            logger::log_error_noexcept("decklink", "DeckLink playback-stopped callback failed");
            return E_FAIL;
        }
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) noexcept final
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;

        if (decklink_iid_matches<IUnknown>(iid) || decklink_iid_matches<IDeckLinkVideoOutputCallback>(iid)) {
            *ppv = static_cast<IDeckLinkVideoOutputCallback*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() noexcept final { return ++ref_count_; }
    ULONG STDMETHODCALLTYPE Release() noexcept final
    {
        const ULONG count = --ref_count_;
        if (count == 0) {
            delete this;
        }
        return count;
    }
};

class node_impl : public node_i
{
    using selection_t = std::tuple<std::string, std::string, keyer_mode_e, bool, frame_rate_s, uint64_t, int>;

    decklink_ptr<callback_s>                  callback_;
    std::optional<callback_s::render_state_s> render_state_;

    std::unique_ptr<output_frame_renderer_i>                  frame_renderer_;
    utils::observed_value_s<selection_t>                      selection_;
    utils::observed_value_s<uint64_t>                         device_version_;
    utils::observed_value_s<std::pair<std::string, uint64_t>> mode_options_version_;
    utils::observed_value_s<std::pair<std::string, uint64_t>> device_status_version_;
    std::chrono::steady_clock::time_point                     next_start_attempt_;
    std::chrono::steady_clock::time_point                     next_metrics_status_;
    uint64_t                                                  render_target_drops_{};
    std::optional<gpu::transfer::texture_readback_target_s>   render_target_;

    input_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    void stop_playback()
    {
        render_target_.reset();
        render_state_.reset();
        frame_renderer_.reset();
        mode_options_version_.reset();
        if (callback_) {
            callback_->stop_async();
            callback_ = nullptr;
        }
    }

    bool start_playback(core::app_state_s*            app,
                        decklink_ptr<IDeckLinkOutput> device,
                        std::string_view              device_name,
                        std::string_view              display_mode,
                        keyer_mode_e                  keyer_mode,
                        utils::flicks                 program_frame_duration,
                        int                           buffer_frames)
    {
        auto reservation = device_reservation_s<IDeckLinkOutput>::acquire(device.get());
        if (!reservation) {
            return false;
        }

        log()->info("Scheduling DeckLink output setup for {}", device_name);
        callback_ = make_decklink_ptr<callback_s>(app->texture_readback_service(),
                                                  app->decklink_registry()->control_executor(),
                                                  std::move(device),
                                                  std::move(reservation),
                                                  std::string(device_name),
                                                  std::string(display_mode),
                                                  keyer_mode,
                                                  program_frame_duration,
                                                  static_cast<size_t>(buffer_frames));
        callback_->start_async();
        return true;
    }

    void publish_device_status(core::app_state_s* app, std::string_view device_name)
    {
        const auto device_status = app->decklink_registry()->get_device_status(device_name);
        const auto status_key    = std::pair(std::string(device_name), device_status ? device_status->version : 0);
        if (device_status_version_.observe(status_key)) {
            app->status_registry()->write(id_, make_device_status(device_status ? *device_status : device_status_s{}));
        }
    }

    void publish_callback_status(core::node_status_registry_s* status_registry, std::string_view device_name)
    {
        if (!callback_) {
            return;
        }

        const auto options_key = std::pair(std::string(device_name), callback_->mode_options_version());
        if (mode_options_version_.observe(options_key)) {
            status_registry->write(id_, status::display_modes_status_s{.display_modes = callback_->mode_options()});
        }
        status_registry->write(id_, callback_->keyer_status());

        const auto now = std::chrono::steady_clock::now();
        if (now < next_metrics_status_) {
            return;
        }
        const auto metrics = callback_->metrics();
        status_registry->write(id_,
                               status::decklink_output_metrics_status_s{
                                   .frames_completed                     = metrics.frames_completed,
                                   .frames_displayed_late                = metrics.frames_displayed_late,
                                   .frames_dropped                       = metrics.frames_dropped,
                                   .frames_flushed                       = metrics.frames_flushed,
                                   .program_frames_received              = metrics.program_frames_received,
                                   .program_queue_overflow_drops         = metrics.program_queue_overflow_drops,
                                   .program_timing_drops                 = metrics.program_timing_drops,
                                   .program_frames_repeated              = metrics.program_frames_repeated,
                                   .program_frames_missing               = metrics.program_frames_missing,
                                   .program_cadence_repeats              = metrics.program_cadence_repeats,
                                   .program_starvation_repeats           = metrics.program_starvation_repeats,
                                   .program_starvation_repeat_streak     = metrics.program_starvation_repeat_streak,
                                   .program_starvation_repeat_streak_max = metrics.program_starvation_repeat_streak_max,
                                   .output_refill_shortfalls             = metrics.output_refill_shortfalls,
                                   .content_frames_sampled               = metrics.content_frames_sampled,
                                   .content_frame_repeats                = metrics.content_frame_repeats,
                                   .content_repeat_streak                = metrics.content_repeat_streak,
                                   .content_repeat_streak_max            = metrics.content_repeat_streak_max,
                                   .completion_intervals                 = metrics.completion_intervals,
                                   .completion_interval_max_us           = metrics.completion_interval_max_us,
                                   .program_queue_depth                  = metrics.program_queue_depth,
                                   .program_queue_depth_max              = metrics.program_queue_depth_max,
                                   .buffered_video_frames                = metrics.buffered_video_frames,
                                   .buffered_video_frames_min            = metrics.buffered_video_frames_min,
                                   .buffered_video_frames_max            = metrics.buffered_video_frames_max,
                                   .buffered_below_target_samples        = metrics.buffered_below_target_samples,
                                   .buffered_zero_samples                = metrics.buffered_zero_samples,
                                   .output_latency_us                    = metrics.output_latency_us,
                                   .program_selection_offset_us          = metrics.program_selection_offset_us,
                                   .completion_time_failures             = metrics.completion_time_failures,
                                   .render_target_drops                  = render_target_drops_,
                               });
        status_registry->write(
            id_,
            status::download_stream_status_s{
                .download_slots                      = metrics.readback_stream.slots,
                .download_slots_free                 = metrics.readback_stream.free_slots,
                .download_slots_rendering            = metrics.readback_stream.rendering_slots,
                .download_slots_queued               = metrics.readback_stream.queued_slots,
                .download_slots_ready                = metrics.readback_stream.ready_slots,
                .download_slots_cpu_reading          = metrics.readback_stream.cpu_reading_slots,
                .download_pending_allocations        = metrics.readback_stream.pending_allocations,
                .download_acquire_misses             = metrics.readback_stream.render_target_acquire_misses,
                .download_transfers_completed        = metrics.readback_stream.transfers_completed,
                .download_transfer_failures          = metrics.readback_stream.transfer_failures,
                .download_transfer_duration_total_us = metrics.readback_stream.transfer_duration_total_us,
                .download_transfer_duration_max_us   = metrics.readback_stream.transfer_duration_max_us,
                .download_allocation_failed          = metrics.readback_stream.allocation_failed,
            });
        next_metrics_status_ = now + 1s;
    }

    bool prepare_existing_playback(core::node_status_registry_s* status, std::string_view device_name)
    {
        if (!callback_) {
            return false;
        }

        const auto phase = callback_->phase();
        if (phase == callback_s::phase_e::prerolling || phase == callback_s::phase_e::running) {
            if (!render_state_) {
                render_state_ = callback_->render_state();
            }
            if (phase == callback_s::phase_e::prerolling) {
                callback_->request_preroll_pump();
            }
            status->write(id_, status::connected_status_s{.connected = render_state_.has_value()});
            return true;
        }

        if (phase == callback_s::phase_e::failed || phase == callback_s::phase_e::stopped) {
            if (phase == callback_s::phase_e::failed) {
                log()->error("DeckLink output playback failed: {}", device_name);
            }
            callback_ = nullptr;
            render_state_.reset();
            frame_renderer_.reset();
            next_start_attempt_ = std::chrono::steady_clock::now() + 1s;
            return false;
        }

        render_state_.reset();
        frame_renderer_.reset();
        status->write(id_, status::connected_status_s{.connected = false});
        return true;
    }

  public:
    ~node_impl() override { stop_playback(); }

    node_impl()                            = default;
    node_impl(const node_impl&)            = delete;
    node_impl& operator=(const node_impl&) = delete;
    node_impl(node_impl&&)                 = delete;
    node_impl& operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, prepare_result_s* result) final
    {
        auto* status = app->status_registry();

        const auto device_list_version = app->decklink_registry()->get_device_list_version();
        const bool device_list_changed = device_version_.observe(device_list_version);
        if (device_list_changed) {
            status->write(
                id_, status::device_names_status_s{.device_names = app->decklink_registry()->get_output_options()});
        }

        const auto device_name    = state.get_option<std::string>("device_name");
        const auto display_mode   = state.get_option<std::string>("display_mode");
        const auto keyer_mode     = state.get_enum_option("keyer_mode", keyer_mode_e::disabled);
        const auto enabled        = state.get_option<bool>("enabled");
        const auto buffer_frames  = app->frame_settings().decklink_output.buffer_frames;
        result->demands_execution = enabled;
        publish_device_status(app, device_name);

        const selection_t selection{
            device_name,
            display_mode,
            keyer_mode,
            enabled,
            app->frame_settings().frame_rate,
            app->frame_context().epoch,
            buffer_frames,
        };
        if (selection_.observe(selection)) {
            stop_playback();
            render_target_drops_ = 0;
            next_start_attempt_  = {};
        }

        if (device_list_changed && callback_ && !app->decklink_registry()->get_output(device_name)) {
            stop_playback();
        }

        if (!callback_) {
            status->write(id_,
                          status::decklink_output_keyer_status_s{
                              .requested_keyer_mode  = std::string(enum_to_string(keyer_mode)),
                              .active_keyer_mode     = std::string(enum_to_string(keyer_mode_e::disabled)),
                              .keyer_fallback_reason = std::nullopt,
                          });
        }
        publish_callback_status(status, device_name);
        if (prepare_existing_playback(status, device_name)) {
            return;
        }

        status->write(id_, status::connected_status_s{.connected = false});
        if (!enabled || std::chrono::steady_clock::now() < next_start_attempt_) {
            return;
        }

        auto device = app->decklink_registry()->get_output(device_name);
        if (!device || !start_playback(app,
                                       std::move(device),
                                       device_name,
                                       display_mode,
                                       keyer_mode,
                                       app->frame_context().frame_duration,
                                       buffer_frames)) {
            next_start_attempt_ = std::chrono::steady_clock::now() + 100ms;
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        render_target_.reset();
        const auto texture = iface_tex_.resolve_value(app, nodes, state);
        if (texture == nullptr || !render_state_) {
            return;
        }

        auto target = render_state_->readback_stream->try_acquire_render_target();
        if (!target) {
            ++render_target_drops_;
            return;
        }

        if (!frame_renderer_) {
            frame_renderer_ = render_state_->active_output.path->create_renderer(app->ctx());
        }
        frame_renderer_->render(texture, *target);
        target->set_program_target_time(app->frame_context().program_target_time);
        render_target_ = std::move(target);
    }

    void complete(core::app_state_s* /*app*/) final
    {
        if (render_target_) {
            render_target_->submit();
            render_target_.reset();
            if (callback_) {
                callback_->request_preroll_pump();
            }
        }
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",         "DeckLink output"                     },
            {"enabled",      true                                  },
            {"display_mode", "720p60"                              },
            {"keyer_mode",   enum_to_string(keyer_mode_e::disabled)},
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "device_name" || name == "display_mode") {
            return normalize_option_value<std::string_view>(value);
        }
        if (name == "keyer_mode") {
            return normalize_enum_option_value<keyer_mode_e>(value);
        }
        if (name == "enabled") {
            return normalize_option_value<bool>(value);
        }
        return option_result_e::invalid;
    }

    std::string_view type() const final { return "decklink_output"; }
};
} // namespace

namespace miximus::nodes::decklink {
std::shared_ptr<node_i> create_output_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::decklink
