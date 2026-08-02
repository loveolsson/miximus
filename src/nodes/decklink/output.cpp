#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/colorspace.hpp"
#include "detail/device_reservation.hpp"
#include "detail/output_video_buffer.hpp"
#include "gpu/color_transfer.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "gpu/transfer/texture_download.hpp"
#include "logger/logger.hpp"
#include "media/frame_fingerprint.hpp"
#include "media/output_buffer_watermark.hpp"
#include "media/output_runtime_metrics.hpp"
#include "media/timed_output_queue.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
#include "types/settings_option.hpp"
#include "utils/observed_value.hpp"
#include "utils/serial_executor.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"
#include "wrapper/decklink-sdk/platform_compat.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
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
constexpr size_t DOWNLOAD_PIPELINE_HEADROOM   = 3;
constexpr size_t RETAINED_PROGRAM_FRAME_COUNT = 1;

constexpr size_t get_output_queue_capacity(size_t buffer_frames) { return buffer_frames + OUTPUT_QUEUE_HEADROOM; }

constexpr size_t get_download_slot_count(size_t scheduled_frames, size_t program_queue_capacity)
{
    // The timed queue retains its current frame separately from queued(), so
    // it needs its own slot in addition to scheduled frames, queued program
    // frames, and the render/download pipeline.
    return scheduled_frames + program_queue_capacity + RETAINED_PROGRAM_FRAME_COUNT + DOWNLOAD_PIPELINE_HEADROOM;
}

auto log() { return getlog("decklink"); }

template <typename T>
nlohmann::json status_value(const std::optional<T>& value)
{
    return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

void write_device_status(core::node_status_registry_s::writer_s& writer, const device_status_s& status)
{
    writer.write("reference_locked", status_value(status.reference_signal_locked));
    writer.write("playback_busy", status_value(status.playback_busy));
    writer.write("pcie_link_width", status_value(status.pcie_link_width));
    writer.write("pcie_link_speed", status_value(status.pcie_link_speed));
    writer.write("temperature_c", status_value(status.temperature_c));
    writer.write("active_format", status_value(status.current_output_mode));
    writer.write("output_pixel_format", status_value(status.last_output_pixel_format));
    writer.write("reference_format", status_value(status.reference_signal_mode));
}

struct mode_info_s
{
    BMDDisplayMode          mode{bmdModeUnknown};
    BMDTimeValue            frame_duration{};
    BMDTimeScale            time_scale{};
    gpu::vec2i_t            dim{};
    gpu::vec2i_t            download_dim{};
    int32_t                 row_bytes{};
    utils::flicks           frame_duration_flicks{};
    gpu::color_conversion_s yuv_conversion{};
    gpu::mat3               gamut_conversion{1.0F};
    BMDColorspace           colorspace{bmdColorspaceRec709};
};

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
        uint64_t                                         completion_interval_max_sequence{};
        size_t                                           program_queue_depth{};
        size_t                                           program_queue_depth_max{};
        uint32_t                                         buffered_video_frames{};
        uint32_t                                         buffered_video_frames_min{};
        uint32_t                                         buffered_video_frames_max{};
        uint64_t                                         buffered_below_target_samples{};
        uint64_t                                         buffered_zero_samples{};
        gpu::transfer::texture_download_stream_metrics_s download_stream;
    };

    struct render_state_s
    {
        std::shared_ptr<gpu::transfer::texture_download_stream_s> download_stream;
        mode_info_s                                               mode;
    };

  private:
    using reservation_s = device_reservation_s<IDeckLinkOutput>;

    std::atomic_ulong ref_count_{1};

    gpu::transfer::texture_download_service_s* download_service_;
    utils::serial_executor_s*                  control_executor_;
    decklink_ptr<IDeckLinkOutput>              device_;
    std::shared_ptr<reservation_s>             reservation_;
    std::string                                device_name_;
    std::string                                requested_mode_name_;

    mutable std::mutex             state_mutex_;
    std::optional<render_state_s>  render_state_;
    std::vector<settings_option_s> mode_options_;
    std::atomic_uint64_t           mode_options_version_;

    mutable std::mutex                                                             frame_mutex_;
    std::shared_ptr<gpu::transfer::texture_download_stream_s>                      download_stream_;
    decklink_ptr<IDeckLinkVideoBuffer>                                             last_buffer_;
    std::optional<media::timed_output_queue_s<decklink_ptr<IDeckLinkVideoBuffer>>> output_queue_;
    media::output_buffer_watermark_s                                               output_watermark_;
    media::output_runtime_metrics_s                                                runtime_metrics_;
    mode_info_s                                                                    mode_info_;
    BMDTimeValue                                                                   pts_{};
    utils::flicks                                                                  program_frame_duration_;
    std::optional<utils::flicks>                                                   next_program_pts_;
    size_t                                                                         configured_buffer_frames_{};
    uint64_t                                                                       program_epoch_{};
    uint64_t                                                                       program_sequence_{};
    uint64_t                                                                       completion_sequence_{};
    uint64_t                                                                       content_frames_sampled_{};
    uint64_t                                                                       content_frame_repeats_{};
    uint64_t                                                                       content_repeat_streak_{};
    uint64_t                                                                       content_repeat_streak_max_{};
    uint64_t                                                                       last_content_fingerprint_{};

    std::atomic_bool     stop_requested_;
    std::atomic_bool     preroll_pump_posted_;
    std::atomic<phase_e> phase_{phase_e::starting};
    bool                 callback_installed_{};
    bool                 output_enabled_{};
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

    auto select_display_mode() -> std::optional<mode_info_s>
    {
        decklink_ptr<IDeckLinkDisplayModeIterator> iterator;
        if (!device_ || device_->GetDisplayModeIterator(iterator.releaseAndGetAddressOf()) != S_OK || !iterator) {
            log()->error("Failed to enumerate display modes for DeckLink output {}", device_name_);
            return std::nullopt;
        }

        std::optional<mode_info_s>         selected;
        std::vector<std::string>           names;
        decklink_ptr<IDeckLinkDisplayMode> sdk_mode;
        while (iterator->Next(sdk_mode.releaseAndGetAddressOf()) == S_OK) {
            mode_info_s mode{};
            mode.mode = sdk_mode->GetDisplayMode();
            mode.dim  = {sdk_mode->GetWidth(), sdk_mode->GetHeight()};
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

            const int row_pixels = ((mode.dim.x + 47) / 48) * 32;
            mode.download_dim    = {row_pixels, mode.dim.y};
            mode.row_bytes       = row_pixels * 4;

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

    void set_colorspace_metadata(IDeckLinkMutableVideoFrame* frame) const
    {
        auto metadata = query_decklink_interface<IDeckLinkVideoFrameMutableMetadataExtensions>(frame);
        if (metadata) {
            (void)metadata->SetInt(bmdDeckLinkFrameMetadataColorspace, mode_info_.colorspace);
        }
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
        if (buffer == nullptr) {
            return {};
        }

        decklink_ptr<IDeckLinkMutableVideoFrame> frame;
        if (device_->CreateVideoFrameWithBuffer(mode_info_.dim.x,
                                                mode_info_.dim.y,
                                                mode_info_.row_bytes,
                                                bmdFormat10BitYUV,
                                                bmdFrameFlagDefault,
                                                buffer,
                                                frame.releaseAndGetAddressOf()) != S_OK) {
            return {};
        }
        set_colorspace_metadata(frame.get());
        return frame.query<IDeckLinkVideoFrame>();
    }

    bool initialize_output()
    {
        const auto mode = select_display_mode();
        if (!mode) {
            return false;
        }

        bool supported{};
        if (device_->DoesSupportVideoMode(bmdVideoConnectionUnspecified,
                                          mode->mode,
                                          bmdFormat10BitYUV,
                                          bmdNoVideoOutputConversion,
                                          bmdSupportedVideoModeDefault,
                                          nullptr,
                                          &supported) != S_OK ||
            !supported) {
            log()->error("DeckLink output mode {} is not supported by {}", requested_mode_name_, device_name_);
            return false;
        }

        if (device_->EnableVideoOutput(mode->mode, bmdVideoOutputFlagDefault) != S_OK) {
            log()->error("Failed to enable DeckLink output {}", device_name_);
            return false;
        }
        output_enabled_ = true;

        const auto device_minimum = minimum_preroll_frames();
        if (configured_buffer_frames_ < device_minimum) {
            log()->info("DeckLink output {} requires at least {} buffered frames; configured {}",
                        device_name_,
                        device_minimum,
                        configured_buffer_frames_);
        }
        const auto buffer_frames = std::max(configured_buffer_frames_, device_minimum);
        output_watermark_        = media::output_buffer_watermark_s(buffer_frames);
        output_queue_.emplace(media::timed_output_queue_config_s{
            .capacity        = get_output_queue_capacity(buffer_frames),
            .early_tolerance = program_frame_duration_ / 2,
        });

        const auto download_slot_count = get_download_slot_count(output_watermark_.target(), output_queue_->capacity());
        const auto stream              = download_service_->create_stream({
                         .requirements =
                {
                               .dimensions  = mode->download_dim,
                               .format      = gpu::texture_s::format_e::uyuv_u10,
                               .row_stride  = static_cast<size_t>(mode->row_bytes),
                               .byte_size   = static_cast<size_t>(mode->row_bytes) * mode->dim.y,
                               .host_access = gpu::transfer::host_access_e::read_only,
                               },
                         .max_slots     = download_slot_count,
                         .initial_slots = download_slot_count,
        });
        if (!stream->wait_for_initial_slots(5s)) {
            log()->error("Failed to initialize the DeckLink output transfer pool for {}", device_name_);
            return false;
        }

        mode_info_       = *mode;
        download_stream_ = stream;
        {
            const std::scoped_lock lock(state_mutex_);
            render_state_.emplace(render_state_s{.download_stream = stream, .mode = *mode});
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
        if (device_ && output_enabled_) {
            (void)device_->DisableVideoOutput();
            output_enabled_ = false;
        }

        {
            const std::scoped_lock lock(frame_mutex_);
            last_buffer_ = nullptr;
            output_queue_.reset();
            next_program_pts_.reset();
            download_stream_.reset();
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
            runtime_metrics_.observe_buffered_frames(buffered, static_cast<uint32_t>(output_watermark_.target()));
        }
    }

    void consume_downloaded_frames()
    {
        if (!output_queue_.has_value()) {
            return;
        }
        auto& output_queue = *output_queue_;

        // The timed queue owns its overflow policy. Drain every completed
        // lease so obsolete queued frames are released instead of exhausting
        // the download stream.
        while (true) {
            auto frame = download_stream_->try_consume_oldest();
            if (!frame.has_value()) {
                break;
            }
            if (frame->bytes().size() != static_cast<size_t>(mode_info_.row_bytes) * mode_info_.dim.y) {
                log()->error("DeckLink output transfer produced an unexpected buffer size");
                continue;
            }
            if (std::cmp_greater(frame->tag(), std::numeric_limits<utils::flicks::rep>::max())) {
                log()->error("DeckLink output transfer has an invalid program PTS");
                continue;
            }

            const auto program_pts = utils::flicks{static_cast<utils::flicks::rep>(frame->tag())};
            const auto fingerprint = media::sampled_frame_fingerprint(frame->bytes());
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
                .id =
                    {
                         .epoch    = program_epoch_,
                         .sequence = program_sequence_++,
                         .pts      = program_pts,
                         .duration = program_frame_duration_,
                         },
                .value = buffer.query<IDeckLinkVideoBuffer>(),
            });
        }
    }

    bool schedule_program_frame(utils::flicks program_pts)
    {
        if (!output_queue_.has_value()) {
            return false;
        }
        auto&      output_queue = *output_queue_;
        const auto selection    = output_queue.select(program_epoch_, program_pts);
        runtime_metrics_.observe_selection(selection.selection, output_queue.queued() != 0);
        if (selection.frame != nullptr) {
            last_buffer_ = selection.frame->value;
        }
        if (!last_buffer_) {
            return false;
        }

        auto frame = create_frame(last_buffer_.get());
        if (!frame ||
            device_->ScheduleVideoFrame(frame.get(), pts_, mode_info_.frame_duration, mode_info_.time_scale) != S_OK) {
            return false;
        }
        output_watermark_.frame_scheduled();
        pts_ += mode_info_.frame_duration;
        return true;
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
            consume_downloaded_frames();
            runtime_metrics_.observe_output_queue_depth(output_queue.queued());
            if (output_queue.queued() < output_watermark_.target()) {
                return;
            }

            next_program_pts_ = output_queue.oldest_pts();
            for (size_t i = 0; i < output_watermark_.target(); ++i) {
                const auto program_pts = next_program_pts_.value_or(utils::flicks{});
                if (!next_program_pts_.has_value() || !schedule_program_frame(program_pts)) {
                    break;
                }
                next_program_pts_ = program_pts + mode_info_.frame_duration_flicks;
            }
            if (output_watermark_.scheduled_frames() != output_watermark_.target()) {
                runtime_metrics_.observe_refill(output_watermark_.target(), output_watermark_.scheduled_frames());
            } else {
                phase_ = phase_e::running;
                if (device_->StartScheduledPlayback(0, mode_info_.time_scale, 1.0) == S_OK) {
                    playback_started_ = true;
                    sample_buffered_video_frames();
                    started = true;
                } else {
                    phase_ = phase_e::prerolling;
                }
            }
        }

        if (started) {
            log()->info("DeckLink output playback running after {} program preroll frames: {}",
                        output_watermark_.target(),
                        device_name_);
        } else {
            log()->error("Failed to start DeckLink output with program-frame preroll: {}", device_name_);
            request_failure();
        }
    }

    HRESULT scheduled_frame_completed()
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

        if (!output_watermark_.frame_completed()) {
            log()->error("DeckLink reported an output completion with no scheduled frame");
            request_failure();
            return E_FAIL;
        }

        runtime_metrics_.observe_completion(std::chrono::steady_clock::now(), ++completion_sequence_);
        sample_buffered_video_frames();
        consume_downloaded_frames();
        const auto refill_count = output_watermark_.refill_count();
        size_t     scheduled{};
        for (size_t i = 0; i < refill_count; ++i) {
            const auto program_pts = next_program_pts_.value_or(utils::flicks{});
            if (!next_program_pts_.has_value() || !schedule_program_frame(program_pts)) {
                break;
            }
            next_program_pts_ = program_pts + mode_info_.frame_duration_flicks;
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
    callback_s(gpu::transfer::texture_download_service_s* download_service,
               utils::serial_executor_s*                  control_executor,
               decklink_ptr<IDeckLinkOutput>              device,
               std::shared_ptr<reservation_s>             reservation,
               std::string                                device_name,
               std::string                                requested_mode_name,
               utils::flicks                              program_frame_duration,
               uint64_t                                   program_epoch,
               size_t                                     buffer_frames)
        : download_service_(download_service)
        , control_executor_(control_executor)
        , device_(std::move(device))
        , reservation_(std::move(reservation))
        , device_name_(std::move(device_name))
        , requested_mode_name_(std::move(requested_mode_name))
        , output_watermark_(buffer_frames)
        , program_frame_duration_(program_frame_duration)
        , configured_buffer_frames_(buffer_frames)
        , program_epoch_(program_epoch)
    {
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
                           .completion_interval_max_sequence = runtime_metrics.completion_interval_max_sequence,
                           .program_queue_depth              = runtime_metrics.output_queue_depth,
                           .program_queue_depth_max          = runtime_metrics.output_queue_depth_max,
                           .buffered_video_frames            = runtime_metrics.buffered_frames,
                           .buffered_video_frames_min        = runtime_metrics.buffered_frames_min,
                           .buffered_video_frames_max        = runtime_metrics.buffered_frames_max,
                           .buffered_below_target_samples    = runtime_metrics.buffered_below_target_samples,
                           .buffered_zero_samples            = runtime_metrics.buffered_zero_samples,
                           .download_stream                  = {},
        };
        if (download_stream_) {
            result.download_stream = download_stream_->metrics();
        }
        return result;
    }

    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* /*completedFrame*/,
                                                      BMDOutputFrameCompletionResult result) noexcept final
    {
        switch (result) {
            case bmdOutputFrameCompleted:
                ++frames_completed_;
                break;
            case bmdOutputFrameDisplayedLate:
                ++frames_displayed_late_;
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
            return scheduled_frame_completed();
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
    using selection_t = std::tuple<std::string, std::string, bool, frame_rate_s, uint64_t, int>;

    decklink_ptr<callback_s>                  callback_;
    std::optional<callback_s::render_state_s> render_state_;

    std::unique_ptr<gpu::framebuffer_s>                       framebuffer_scale_;
    std::unique_ptr<gpu::textured_quad_s>                     textured_quad_yuv_;
    std::unique_ptr<gpu::textured_quad_s>                     textured_quad_scale_;
    utils::observed_value_s<selection_t>                      selection_;
    utils::observed_value_s<uint64_t>                         device_version_;
    utils::observed_value_s<std::pair<std::string, uint64_t>> mode_options_version_;
    utils::observed_value_s<std::pair<std::string, uint64_t>> device_status_version_;
    std::chrono::steady_clock::time_point                     next_start_attempt_;
    std::chrono::steady_clock::time_point                     next_metrics_status_;
    uint64_t                                                  render_target_drops_{};

    input_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    void stop_playback()
    {
        render_state_.reset();
        framebuffer_scale_.reset();
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
                        utils::flicks                 program_frame_duration,
                        uint64_t                      program_epoch,
                        int                           buffer_frames)
    {
        auto reservation = device_reservation_s<IDeckLinkOutput>::acquire(device.get());
        if (!reservation) {
            return false;
        }

        log()->info("Scheduling DeckLink output setup for {}", device_name);
        callback_ = make_decklink_ptr<callback_s>(app->texture_download_service(),
                                                  app->decklink_registry()->control_executor(),
                                                  std::move(device),
                                                  std::move(reservation),
                                                  std::string(device_name),
                                                  std::string(display_mode),
                                                  program_frame_duration,
                                                  program_epoch,
                                                  static_cast<size_t>(buffer_frames));
        callback_->start_async();
        return true;
    }

    void publish_device_status(core::app_state_s* app, std::string_view device_name)
    {
        const auto device_status = app->decklink_registry()->get_device_status(device_name);
        const auto status_key    = std::pair(std::string(device_name), device_status ? device_status->version : 0);
        if (device_status_version_.observe(status_key)) {
            auto writer = app->status_registry()->write_node(id_);
            write_device_status(writer, device_status ? *device_status : device_status_s{});
        }
    }

    void publish_callback_status(core::node_status_registry_s* status_registry, std::string_view device_name)
    {
        if (!callback_) {
            return;
        }

        const auto options_key = std::pair(std::string(device_name), callback_->mode_options_version());
        if (mode_options_version_.observe(options_key)) {
            status_registry->write(id_, "display_modes", callback_->mode_options());
        }

        const auto now = std::chrono::steady_clock::now();
        if (now < next_metrics_status_) {
            return;
        }
        const auto metrics = callback_->metrics();
        auto       writer  = status_registry->write_node(id_);
        writer.write("frames_completed", metrics.frames_completed);
        writer.write("frames_displayed_late", metrics.frames_displayed_late);
        writer.write("frames_dropped", metrics.frames_dropped);
        writer.write("frames_flushed", metrics.frames_flushed);
        writer.write("program_frames_received", metrics.program_frames_received);
        writer.write("program_queue_overflow_drops", metrics.program_queue_overflow_drops);
        writer.write("program_timing_drops", metrics.program_timing_drops);
        writer.write("program_frames_repeated", metrics.program_frames_repeated);
        writer.write("program_frames_missing", metrics.program_frames_missing);
        writer.write("program_cadence_repeats", metrics.program_cadence_repeats);
        writer.write("program_starvation_repeats", metrics.program_starvation_repeats);
        writer.write("program_starvation_repeat_streak", metrics.program_starvation_repeat_streak);
        writer.write("program_starvation_repeat_streak_max", metrics.program_starvation_repeat_streak_max);
        writer.write("output_refill_shortfalls", metrics.output_refill_shortfalls);
        writer.write("content_frames_sampled", metrics.content_frames_sampled);
        writer.write("content_frame_repeats", metrics.content_frame_repeats);
        writer.write("content_repeat_streak", metrics.content_repeat_streak);
        writer.write("content_repeat_streak_max", metrics.content_repeat_streak_max);
        writer.write("completion_intervals", metrics.completion_intervals);
        writer.write("completion_interval_max_us", metrics.completion_interval_max_us);
        writer.write("completion_interval_max_sequence", metrics.completion_interval_max_sequence);
        writer.write("program_queue_depth", metrics.program_queue_depth);
        writer.write("program_queue_depth_max", metrics.program_queue_depth_max);
        writer.write("buffered_video_frames", metrics.buffered_video_frames);
        writer.write("buffered_video_frames_min", metrics.buffered_video_frames_min);
        writer.write("buffered_video_frames_max", metrics.buffered_video_frames_max);
        writer.write("buffered_below_target_samples", metrics.buffered_below_target_samples);
        writer.write("buffered_zero_samples", metrics.buffered_zero_samples);
        writer.write("download_slots", metrics.download_stream.slots);
        writer.write("download_slots_free", metrics.download_stream.free_slots);
        writer.write("download_slots_rendering", metrics.download_stream.rendering_slots);
        writer.write("download_slots_queued", metrics.download_stream.queued_slots);
        writer.write("download_slots_ready", metrics.download_stream.ready_slots);
        writer.write("download_slots_cpu_reading", metrics.download_stream.cpu_reading_slots);
        writer.write("download_pending_allocations", metrics.download_stream.pending_allocations);
        writer.write("download_acquire_misses", metrics.download_stream.acquire_misses);
        writer.write("download_transfers_completed", metrics.download_stream.transfers_completed);
        writer.write("download_transfer_failures", metrics.download_stream.transfer_failures);
        writer.write("download_transfer_duration_total_us", metrics.download_stream.transfer_duration_total_us);
        writer.write("download_transfer_duration_max_us", metrics.download_stream.transfer_duration_max_us);
        writer.write("download_allocation_failed", metrics.download_stream.allocation_failed);
        writer.write("render_target_drops", render_target_drops_);
        next_metrics_status_ = now + 1s;
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
            status->write(id_, "device_names", app->decklink_registry()->get_output_options());
        }

        const auto device_name    = state.get_option<std::string>("device_name");
        const auto display_mode   = state.get_option<std::string>("display_mode");
        const auto enabled        = state.get_option<bool>("enabled");
        const auto buffer_frames  = app->frame_settings().decklink_output.buffer_frames;
        result->demands_execution = enabled;
        publish_device_status(app, device_name);

        const selection_t selection{
            device_name,
            display_mode,
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

        publish_callback_status(status, device_name);
        if (callback_) {
            const auto phase = callback_->phase();
            if (phase == callback_s::phase_e::prerolling || phase == callback_s::phase_e::running) {
                if (!render_state_) {
                    render_state_ = callback_->render_state();
                }
                if (phase == callback_s::phase_e::prerolling) {
                    callback_->request_preroll_pump();
                }
                status->write(id_, "connected", render_state_.has_value());
                return;
            }
            if (phase == callback_s::phase_e::failed || phase == callback_s::phase_e::stopped) {
                if (phase == callback_s::phase_e::failed) {
                    log()->error("DeckLink output playback failed: {}", device_name);
                }
                callback_ = nullptr;
                render_state_.reset();
                framebuffer_scale_.reset();
                next_start_attempt_ = std::chrono::steady_clock::now() + 1s;
            } else {
                render_state_.reset();
                framebuffer_scale_.reset();
                status->write(id_, "connected", false);
                return;
            }
        }

        status->write(id_, "connected", false);
        if (!enabled || std::chrono::steady_clock::now() < next_start_attempt_) {
            return;
        }

        auto device = app->decklink_registry()->get_output(device_name);
        if (!device || !start_playback(app,
                                       std::move(device),
                                       device_name,
                                       display_mode,
                                       app->frame_context().duration,
                                       app->frame_context().epoch,
                                       buffer_frames)) {
            next_start_attempt_ = std::chrono::steady_clock::now() + 100ms;
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        const auto texture = iface_tex_.resolve_value(app, nodes, state);
        if (texture == nullptr || !render_state_) {
            return;
        }

        auto target = render_state_->download_stream->try_acquire();
        if (!target) {
            ++render_target_drops_;
            return;
        }

        if (!textured_quad_scale_) {
            textured_quad_scale_ =
                std::make_unique<gpu::textured_quad_s>(app->ctx()->get_shader(gpu::shader_program_s::name_e::basic));
        }
        if (!textured_quad_yuv_) {
            textured_quad_yuv_ = std::make_unique<gpu::textured_quad_s>(
                app->ctx()->get_shader(gpu::shader_program_s::name_e::rgb_to_yuv));
        }

        const auto& mode = render_state_->mode;
        if (!framebuffer_scale_ || framebuffer_scale_->texture()->texture_dimensions() != mode.dim) {
            framebuffer_scale_ = std::make_unique<gpu::framebuffer_s>(mode.dim, gpu::texture_s::format_e::rgb_f16);
        }

        framebuffer_scale_->begin_render(gpu::framebuffer_s::load_op_e::clear);
        textured_quad_scale_->draw(texture);
        gpu::framebuffer_s::end_render();

        target->framebuffer()->begin_render(
            {
                .pos  = {0, 0},
                .size = mode.download_dim,
        },
            gpu::framebuffer_s::load_op_e::clear);

        auto shader = textured_quad_yuv_->shader();
        shader->set_uniform("target_width", mode.download_dim.x);
        shader->set_uniform("transfer", mode.yuv_conversion.matrix);
        shader->set_uniform("transfer_offset", mode.yuv_conversion.offset);
        shader->set_uniform("gamut_transfer", mode.gamut_conversion);
        textured_quad_yuv_->draw(framebuffer_scale_->texture());

        gpu::framebuffer_s::end_render();
        target->set_tag(static_cast<uint64_t>(app->frame_context().pts.count()));
        target->submit();
        callback_->request_preroll_pump();
    }

    void complete(core::app_state_s* /*app*/) final {}

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",         "DeckLink output"},
            {"enabled",      true             },
            {"display_mode", "720p60"         },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "device_name" || name == "display_mode") {
            return normalize_option_value<std::string_view>(value);
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
