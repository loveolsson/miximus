#include "input_capture.hpp"

#include "allocator.hpp"
#include "colorspace.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "logger/logger.hpp"
#include "media/frame_fingerprint.hpp"
#include "wrapper/decklink-sdk/platform_compat.hpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <mutex>
#include <utility>

namespace {
using namespace miximus;
using namespace miximus::decklink_sdk;
using namespace miximus::nodes::decklink::detail;

constexpr size_t SOURCE_PLAYOUT_DELAY_FRAMES = 3;
// Selection happens on the render thread after the SDK callback may have
// delivered the next frame. Account for both the frame becoming eligible and
// that concurrent ingress frame in addition to the delayed frames.
constexpr size_t SOURCE_QUEUE_CAPACITY = SOURCE_PLAYOUT_DELAY_FRAMES + 2;

auto log() { return getlog("decklink"); }

struct captured_frame_data_s
{
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
    std::optional<gpu::transfer::texture_upload_lease_s>    upload;
    decklink_ptr<IDeckLinkVideoInputFrame>                  input_frame;
    uint64_t                                                upload_version{};
    gpu::texture_frame_ptr                                  frame;
    gpu::vec2i_t                                            src_dim{};
    BMDColorspace                                           colorspace{bmdColorspaceRec709};
};

// ─── callback_s ──────────────────────────────────────────────────────────────
// Implements both IDeckLinkInputCallback (capture events) and
// IDeckLinkVideoBufferAllocatorProvider (provides our transfer-backed buffers
// so DeckLink DMA's directly into our memory instead of its own).

// The DeckLink SDK exposes these two independent COM interfaces on one callback object.
// NOLINTNEXTLINE(fuchsia-multiple-inheritance)
class callback_s
    : public IDeckLinkInputCallback
    , public IDeckLinkVideoBufferAllocatorProvider
{
  public:
    using phase_e = input_capture_s::phase_e;

  private:
    std::atomic_ulong ref_count_{1};

    gpu::transfer::texture_upload_service_s*                upload_service_;
    utils::serial_executor_s*                               control_executor_;
    decklink_ptr<IDeckLinkInput>                            device_;
    std::shared_ptr<device_reservation_s<IDeckLinkInput>>   reservation_;
    std::string                                             device_name_;
    decklink_ptr<input_video_buffer_allocator_s>            allocator_;
    mutable std::mutex                                      upload_mutex_;
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream_;
    media::timed_source_queue_s<captured_frame_data_s>      frame_queue_{
             {.capacity = SOURCE_QUEUE_CAPACITY, .playout_delay_frames = SOURCE_PLAYOUT_DELAY_FRAMES}
    };

    std::atomic<BMDDisplayMode> pending_display_mode_{bmdModeUnknown};
    std::atomic<BMDColorspace>  colorspace_{bmdColorspaceRec709};
    std::atomic_bool            stop_requested_{false};
    std::atomic<phase_e>        phase_{phase_e::starting};
    std::atomic<phase_e>        terminal_phase_{phase_e::stopped};
    std::atomic<BMDDisplayMode> active_display_mode_{bmdModeUnknown};
    bool                        capture_active_{};

    std::atomic_uint64_t                  frames_received_;
    std::atomic_uint64_t                  frames_missing_;
    std::atomic_uint64_t                  no_input_source_frames_;
    std::atomic_uint64_t                  upload_slot_drops_;
    std::atomic_uint64_t                  content_frames_sampled_;
    std::atomic_uint64_t                  content_frame_repeats_;
    std::atomic_uint64_t                  content_repeat_streak_;
    std::atomic_uint64_t                  content_repeat_streak_max_;
    std::atomic_uint64_t                  last_content_fingerprint_;
    std::atomic_uint32_t                  available_video_frames_{0};
    std::chrono::steady_clock::time_point next_queue_poll_;
    std::chrono::steady_clock::time_point next_upload_slot_warning_;
    BMDTimeValue                          last_stream_time_{};
    bool                                  has_stream_time_{};
    BMDTimeValue                          hardware_reference_origin_{};
    utils::flicks                         local_reference_origin_{};
    bool                                  has_hardware_reference_{};
    std::atomic_bool                      reset_stream_time_;
    std::atomic_bool                      warned_missing_custom_buffer_;
    std::atomic_uint64_t                  source_epoch_;
    std::atomic_uint64_t                  next_sequence_;
    bool                                  warned_submit_failure_{};
    bool                                  warned_wait_failure_{};
    bool                                  warned_consume_failure_{};
    bool                                  warned_commit_failure_{};

    void observe_frame_content(std::span<const std::byte> bytes)
    {
        const auto fingerprint = media::sampled_frame_fingerprint(bytes);
        const auto previous    = last_content_fingerprint_.exchange(fingerprint);
        const auto sampled     = content_frames_sampled_.fetch_add(1);
        if (sampled != 0 && previous == fingerprint) {
            ++content_frame_repeats_;
            const auto streak  = content_repeat_streak_.fetch_add(1) + 1;
            auto       maximum = content_repeat_streak_max_.load();
            while (streak > maximum && !content_repeat_streak_max_.compare_exchange_weak(maximum, streak)) {
            }
        } else {
            content_repeat_streak_ = 0;
        }
    }

    BMDColorspace get_frame_colorspace(IDeckLinkVideoInputFrame* frame) const
    {
        auto metadata = query_decklink_interface<IDeckLinkVideoFrameMetadataExtensions>(frame);
        if (metadata) {
            int64_t value = 0;
            if (metadata->GetInt(bmdDeckLinkFrameMetadataColorspace, &value) == S_OK) {
                const auto colorspace = static_cast<BMDColorspace>(value);
                if (colorspace == bmdColorspaceRec601 || colorspace == bmdColorspaceRec709 ||
                    colorspace == bmdColorspaceRec2020) {
                    return colorspace;
                }
                return colorspace_.load();
            }
        }

        return colorspace_.load();
    }

    void post_control(std::function<void(callback_s&)> task)
    {
        decklink_ptr<callback_s> self(this);
        const bool accepted = control_executor_->post([self = std::move(self), task = std::move(task)]() mutable {
            try {
                task(*self);
            } catch (const std::exception& error) {
                log()->error("DeckLink input control task failed: {}", error.what());
                self->terminal_phase_ = phase_e::failed;
                self->stop_requested_ = true;
                self->phase_          = phase_e::release_requested;
            } catch (...) {
                log()->error("DeckLink input control task failed");
                self->terminal_phase_ = phase_e::failed;
                self->stop_requested_ = true;
                self->phase_          = phase_e::release_requested;
            }
        });
        if (!accepted) {
            log()->error("DeckLink input control executor rejected a task during shutdown");
            phase_ = phase_e::failed;
        }
    }

    void release_upload_pool()
    {
        decklink_ptr<input_video_buffer_allocator_s> allocator;
        {
            const std::scoped_lock lock(upload_mutex_);
            allocator = allocator_;
        }

        if (allocator) {
            allocator->shutdown_and_wait();
        }

        const std::scoped_lock lock(upload_mutex_);
        upload_stream_.reset();
        allocator_ = nullptr;
    }

    void stop_sdk_capture()
    {
        if (!device_) {
            capture_active_ = false;
            return;
        }

        if (capture_active_ && device_->StopStreams() != S_OK) {
            log()->warn("DeckLink input StopStreams failed during asynchronous teardown");
        }
        capture_active_ = false;
        if (device_->SetCallback(nullptr) != S_OK) {
            log()->warn("DeckLink input SetCallback(nullptr) failed during asynchronous teardown");
        }
        if (device_->DisableVideoInput() != S_OK) {
            log()->warn("DeckLink input DisableVideoInput failed during asynchronous teardown");
        }
    }

    bool enable_capture(BMDDisplayMode mode)
    {
        const auto enabled = device_->EnableVideoInputWithAllocatorProvider(
            mode, bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection, this);
        if (enabled != S_OK) {
            log()->error("Failed to enable DeckLink input mode ({:#x})", enabled);
            return false;
        }
        if (device_->StartStreams() != S_OK) {
            log()->error("Failed to start DeckLink input streams");
            (void)device_->DisableVideoInput();
            release_upload_pool();
            return false;
        }

        active_display_mode_ = mode;
        capture_active_      = true;
        reset_stream_time_   = true;
        source_epoch_.fetch_add(1);
        next_sequence_ = 0;
        phase_         = phase_e::running;
        log()->info("DeckLink input capture running: {}", device_name_);
        return true;
    }

    bool start_capture()
    {
        decklink_ptr<IDeckLinkDisplayModeIterator> mode_iterator;
        decklink_ptr<IDeckLinkDisplayMode>         initial_mode;
        if (!device_ || device_->GetDisplayModeIterator(mode_iterator.releaseAndGetAddressOf()) != S_OK ||
            !mode_iterator || mode_iterator->Next(initial_mode.releaseAndGetAddressOf()) != S_OK || !initial_mode) {
            log()->error("Failed to select an initial DeckLink input mode");
            return false;
        }
        if (device_->SetCallback(this) != S_OK) {
            log()->error("Failed to install DeckLink input callback");
            return false;
        }
        return enable_capture(initial_mode->GetDisplayMode());
    }

    bool reconfigure_capture(BMDDisplayMode mode)
    {
        log()->info("Reconfiguring DeckLink input after detected format change: {}", device_name_);
        phase_ = phase_e::reconfiguring;

        if (capture_active_ && device_->StopStreams() != S_OK) {
            log()->warn("DeckLink input StopStreams failed during format change");
        }
        capture_active_ = false;
        if (device_->DisableVideoInput() != S_OK) {
            log()->warn("DeckLink input DisableVideoInput failed during format change");
        }

        // DisableVideoInput relinquishes the provider. Buffer objects may be
        // returned on SDK threads after that call, so retirement waits here on
        // the capture-control thread, never on the render thread.
        release_upload_pool();
        return enable_capture(mode);
    }

    void retire_capture(phase_e final_phase)
    {
        phase_ = phase_e::stopping;
        stop_sdk_capture();
        release_upload_pool();
        device_ = nullptr;
        reservation_.reset();
        phase_ = final_phase;
    }

    void start_capture_control()
    {
        if (stop_requested_.load()) {
            retire_capture(phase_e::stopped);
        } else if (!start_capture()) {
            stop_requested_ = true;
            retire_capture(phase_e::failed);
        } else if (stop_requested_.load()) {
            retire_capture(terminal_phase_.load());
        }
    }

    void reconfigure_capture_control()
    {
        if (stop_requested_.load()) {
            retire_capture(terminal_phase_.load());
            return;
        }

        const auto mode = pending_display_mode_.exchange(bmdModeUnknown);
        if (mode == bmdModeUnknown || mode == active_display_mode_.load()) {
            phase_ = phase_e::running;
            return;
        }

        if (!reconfigure_capture(mode)) {
            stop_requested_ = true;
            retire_capture(phase_e::failed);
        } else if (stop_requested_.load()) {
            retire_capture(phase_e::stopped);
        }
    }

  public:
    using frame_ticket_t = media::timed_source_queue_s<captured_frame_data_s>::ticket_t;

    struct metrics_s
    {
        uint64_t                            frames_received{};
        uint64_t                            frames_missing{};
        uint64_t                            no_input_source_frames{};
        uint64_t                            upload_slot_drops{};
        uint64_t                            upload_acquire_slow_count{};
        uint64_t                            upload_acquire_failures{};
        uint64_t                            upload_acquire_wait_max_us{};
        uint64_t                            content_frames_sampled{};
        uint64_t                            content_frame_repeats{};
        uint64_t                            content_repeat_streak{};
        uint64_t                            content_repeat_streak_max{};
        uint32_t                            available_video_frames{};
        media::timed_source_queue_metrics_s source_queue;
    };

    callback_s(gpu::transfer::texture_upload_service_s*              upload_service,
               utils::serial_executor_s*                             control_executor,
               decklink_ptr<IDeckLinkInput>                          device,
               std::shared_ptr<device_reservation_s<IDeckLinkInput>> reservation,
               std::string                                           device_name)
        : upload_service_(upload_service)
        , control_executor_(control_executor)
        , device_(std::move(device))
        , reservation_(std::move(reservation))
        , device_name_(std::move(device_name))
    {
    }

    ~callback_s() override
    {
        if (allocator_) {
            log()->error("DeckLink input callback destroyed before its allocator was retired");
        }
    }

    callback_s(const callback_s&)            = delete;
    callback_s& operator=(const callback_s&) = delete;
    callback_s(callback_s&&)                 = delete;
    callback_s& operator=(callback_s&&)      = delete;

    // ── IDeckLinkVideoBufferAllocatorProvider ────────────────────────────────

    HRESULT STDMETHODCALLTYPE GetVideoBufferAllocator(uint32_t bufferSize,
                                                      uint32_t /*width*/,
                                                      uint32_t                        height,
                                                      uint32_t                        rowBytes,
                                                      BMDPixelFormat                  pixelFormat,
                                                      IDeckLinkVideoBufferAllocator** outAllocator) noexcept final
    {
        try {
            if (outAllocator == nullptr) {
                return E_POINTER;
            }
            *outAllocator = nullptr;

            if (stop_requested_.load()) {
                return E_FAIL;
            }

            // Only hand over our allocator for the primary capture format.
            // Conversion frames (different pixel format) use DeckLink's default.
            if (pixelFormat != bmdFormat10BitYUV) {
                return E_NOTIMPL;
            }

            {
                const std::scoped_lock lock(upload_mutex_);
                if (allocator_) {
                    if (allocator_->buffer_size() != bufferSize) {
                        log()->error("DeckLink requested two differently sized allocator pools for one capture mode");
                        return E_FAIL;
                    }
                    allocator_->AddRef();
                    *outAllocator = allocator_.get();
                    return S_OK;
                }
            }

            const gpu::transfer::texture_transfer_requirements_s requirements{
                .dimensions        = {static_cast<int>(rowBytes / 4), static_cast<int>(height)},
                .format            = gpu::texture_s::format_e::uyuv_u10,
                .row_stride        = static_cast<size_t>(rowBytes),
                .byte_size         = bufferSize,
                .address_alignment = 64,
                .host_access       = gpu::transfer::host_access_e::overwrite,
            };
            const auto stream = upload_service_->create_stream({
                .requirements = requirements,
                // Timed selection may retain five completed DMA writes while
                // DeckLink continues cycling its independent buffer objects.
                // Additional slots cover the published texture and asynchronous
                // upload/reclaim without making StartAccess wait on rendering.
                .max_slots         = input_video_buffer_allocator_s::UPLOAD_SLOT_COUNT,
                .initial_slots     = input_video_buffer_allocator_s::INITIAL_UPLOAD_SLOT_COUNT,
                .generate_mip_maps = false,
            });

            auto allocator = make_decklink_ptr<input_video_buffer_allocator_s>(bufferSize, stream);
            {
                const std::scoped_lock lock(upload_mutex_);
                if (allocator_) {
                    log()->error("DeckLink requested a new allocator before the previous pool was released");
                    return E_FAIL;
                }
                upload_stream_ = stream;
                allocator_     = std::move(allocator);
            }
            allocator_->AddRef();
            *outAllocator = allocator_.get();
            return S_OK;
        } catch (const std::exception& error) {
            logger::log_error_noexcept("decklink", "Failed to create DeckLink input transfer stream: {}", error.what());
        } catch (...) {
            logger::log_error_noexcept("decklink", "Failed to create DeckLink input transfer stream");
        }
        return E_OUTOFMEMORY;
    }

    // ── IDeckLinkInputCallback ───────────────────────────────────────────────

    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame,
                                                     IDeckLinkAudioInputPacket* /*audioPacket*/) noexcept final
    {
        if (phase_.load() != phase_e::running) {
            return S_OK;
        }

        try {
            return video_input_frame_arrived(videoFrame);
        } catch (const std::exception& error) {
            logger::log_error_noexcept("decklink", "DeckLink input frame callback failed: {}", error.what());
        } catch (...) {
            logger::log_error_noexcept("decklink", "DeckLink input frame callback failed");
        }
        if (!stop_requested_.exchange(true)) {
            terminal_phase_ = phase_e::failed;
            phase_          = phase_e::release_requested;
        }
        return E_FAIL;
    }

  private:
    bool
    read_frame_timing(IDeckLinkVideoInputFrame* video_frame, BMDTimeValue* stream_time, BMDTimeValue* frame_duration)
    {
        constexpr auto time_scale = static_cast<BMDTimeScale>(utils::k_flicks_one_second.count());

        if (reset_stream_time_.exchange(false)) {
            has_stream_time_        = false;
            has_hardware_reference_ = false;
        }
        if (video_frame->GetStreamTime(stream_time, frame_duration, time_scale) != S_OK || *frame_duration <= 0) {
            return false;
        }
        if (has_stream_time_ && *stream_time > last_stream_time_ + *frame_duration) {
            const auto missing = ((*stream_time - last_stream_time_) / *frame_duration) - 1;
            if (missing < 100) {
                frames_missing_.fetch_add(static_cast<uint64_t>(missing));
            }
        }
        last_stream_time_ = *stream_time;
        has_stream_time_  = true;
        return true;
    }

    utils::flicks get_frame_observation(IDeckLinkVideoInputFrame* video_frame, utils::flicks arrival_time)
    {
        constexpr auto time_scale = static_cast<BMDTimeScale>(utils::k_flicks_one_second.count());

        BMDTimeValue hardware_reference_time{};
        BMDTimeValue hardware_reference_duration{};
        if (video_frame->GetHardwareReferenceTimestamp(
                time_scale, &hardware_reference_time, &hardware_reference_duration) != S_OK ||
            hardware_reference_duration <= 0) {
            return arrival_time;
        }

        const auto frame_start = hardware_reference_time - hardware_reference_duration;
        if (!has_hardware_reference_) {
            hardware_reference_origin_ = frame_start;
            local_reference_origin_    = arrival_time;
            has_hardware_reference_    = true;
        }
        return local_reference_origin_ + utils::flicks{frame_start - hardware_reference_origin_};
    }

    void sample_available_video_frames(std::chrono::steady_clock::time_point now)
    {
        if (now < next_queue_poll_) {
            return;
        }

        uint32_t available{};
        if (device_->GetAvailableVideoFrameCount(&available) == S_OK) {
            available_video_frames_ = available;
        }
        next_queue_poll_ = now + std::chrono::seconds(1);
    }

    HRESULT video_input_frame_arrived(IDeckLinkVideoInputFrame* videoFrame)
    {
        const auto frame_arrival_time = utils::flicks_now();

        if (videoFrame == nullptr) {
            return S_OK;
        }

        ++frames_received_;
        const bool has_no_input_source = (videoFrame->GetFlags() & bmdFrameHasNoInputSource) != 0;
        if (has_no_input_source) {
            ++no_input_source_frames_;
        }

        BMDTimeValue stream_time{};
        BMDTimeValue frame_duration{};
        if (!read_frame_timing(videoFrame, &stream_time, &frame_duration)) {
            ++upload_slot_drops_;
            return S_OK;
        }

        const auto frame_observation = get_frame_observation(videoFrame, frame_arrival_time);

        if (has_no_input_source) {
            return S_OK;
        }

        const auto now = std::chrono::steady_clock::now();
        sample_available_video_frames(now);

        const gpu::vec2i_t src_dim{videoFrame->GetWidth(), videoFrame->GetHeight()};
        auto               video_buffer  = query_decklink_interface<IDeckLinkVideoBuffer>(videoFrame);
        auto               upload_buffer = query_input_video_buffer(video_buffer.get());

        input_video_buffer_allocator_s*                         allocator{};
        std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
        {
            const std::scoped_lock lock(upload_mutex_);
            allocator = allocator_.get();
            stream    = upload_stream_;
        }

        if (!upload_buffer || upload_buffer->allocator() != allocator || !stream) {
            ++upload_slot_drops_;
            if (!upload_buffer && !warned_missing_custom_buffer_.exchange(true)) {
                log()->warn("DeckLink returned a frame that was not allocated by its active custom allocator");
            }
            return S_OK;
        }

        auto upload = upload_buffer->take_upload();
        if (!upload.has_value()) {
            ++upload_slot_drops_;
            if (now >= next_upload_slot_warning_) {
                log()->warn("VideoInputFrameArrived dropped frame: no upload slot ({} total)",
                            upload_slot_drops_.load());
                next_upload_slot_warning_ = std::chrono::steady_clock::now() + std::chrono::seconds(1);
            }
            return S_OK;
        }
        observe_frame_content(upload->bytes());
        const auto version = upload->version();

        const media::media_frame_id_s frame_id{
            .epoch    = source_epoch_.load(),
            .sequence = next_sequence_.fetch_add(1),
            .pts      = utils::flicks{stream_time},
            .duration = utils::flicks{frame_duration},
        };
        frame_queue_.push(frame_queue_.create_frame(frame_id,
                                                    frame_observation,
                                                    captured_frame_data_s{
                                                        .stream         = std::move(stream),
                                                        .upload         = std::move(upload),
                                                        .input_frame    = decklink_ptr(videoFrame),
                                                        .upload_version = version,
                                                        .frame          = nullptr,
                                                        .src_dim        = src_dim,
                                                        .colorspace     = get_frame_colorspace(videoFrame),
                                                    }));
        return S_OK;
    }

  public:
    HRESULT STDMETHODCALLTYPE
    VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                            IDeckLinkDisplayMode*            newDisplayMode,
                            BMDDetectedVideoInputFormatFlags /*detectedSignalFlags*/) noexcept final
    {
        if (stop_requested_.load()) {
            return S_OK;
        }
        if (newDisplayMode == nullptr) {
            return E_INVALIDARG;
        }

        const bool display_mode_changed = (notificationEvents & bmdVideoInputDisplayModeChanged) != 0;
        const bool colorspace_changed   = (notificationEvents & bmdVideoInputColorspaceChanged) != 0;

        if (display_mode_changed) {
            const auto mode = newDisplayMode->GetDisplayMode();
            if (mode != active_display_mode_.load()) {
                pending_display_mode_ = mode;
                phase_                = phase_e::release_requested;
            }
        }
        if (display_mode_changed || colorspace_changed) {
            colorspace_ = get_display_mode_colorspace(newDisplayMode);
        }
        return S_OK;
    }

    void advance_frames(utils::flicks program_pts, utils::flicks target_time, bool discontinuity)
    {
        frame_queue_.advance(program_pts, target_time, discontinuity);
    }

    frame_ticket_t select_frame(utils::flicks program_pts, utils::flicks early_tolerance)
    {
        return frame_queue_.select(program_pts, early_tolerance);
    }

    bool submit_frame(frame_ticket_t& ticket)
    {
        if (ticket.selection() == media::prepared_frame_selection_e::repeat) {
            return true;
        }
        if (ticket.selection() != media::prepared_frame_selection_e::new_frame || ticket.frame() == nullptr) {
            return false;
        }

        auto& frame = *ticket.frame();
        auto& info  = frame.value();
        if (frame.readiness() == media::source_frame_readiness_e::submitted) {
            return true;
        }
        if (!frame.mark_submitted() || !info.upload.has_value() || !info.upload->submit()) {
            if (!warned_submit_failure_) {
                log()->warn("DeckLink timed input failed to submit upload version {} (readiness {}, lease {})",
                            info.upload_version,
                            static_cast<int>(frame.readiness()),
                            info.upload.has_value());
                warned_submit_failure_ = true;
            }
            frame_queue_.fail(ticket);
            return false;
        }
        info.upload.reset();
        return true;
    }

    captured_frame_data_s* resolve_frame(frame_ticket_t& ticket)
    {
        if (ticket.frame() == nullptr) {
            return nullptr;
        }

        auto& frame = *ticket.frame();
        auto& info  = frame.value();
        if (ticket.selection() == media::prepared_frame_selection_e::new_frame) {
            const auto wait_result = info.stream ? info.stream->wait_until_ready(info.upload_version)
                                                 : gpu::transfer::texture_upload_wait_result_e::stopped;
            if (wait_result != gpu::transfer::texture_upload_wait_result_e::ready) {
                if (!warned_wait_failure_) {
                    log()->warn("DeckLink timed input failed waiting for upload version {} (result {})",
                                info.upload_version,
                                static_cast<int>(wait_result));
                    warned_wait_failure_ = true;
                }
                frame_queue_.fail(ticket);
                return nullptr;
            }
            info.frame = info.stream->consume_exact(info.upload_version);
            if (!info.frame || info.stream->current_version() != info.upload_version || !frame.mark_ready()) {
                if (!warned_consume_failure_) {
                    log()->warn("DeckLink timed input failed to consume upload version {} (current {}, texture {}, "
                                "readiness {})",
                                info.upload_version,
                                info.stream->current_version(),
                                static_cast<bool>(info.frame),
                                static_cast<int>(frame.readiness()));
                    warned_consume_failure_ = true;
                }
                frame_queue_.fail(ticket);
                return nullptr;
            }
            info.input_frame = nullptr;
        } else if (ticket.selection() == media::prepared_frame_selection_e::repeat) {
            info.frame = info.stream ? info.stream->current_frame(info.upload_version) : nullptr;
            if (!info.frame) {
                frame_queue_.fail(ticket);
                return nullptr;
            }
        }

        if (!ticket.await() || !frame_queue_.commit(ticket)) {
            if (!warned_commit_failure_) {
                log()->warn("DeckLink timed input failed to commit upload version {} (readiness {})",
                            info.upload_version,
                            static_cast<int>(frame.readiness()));
                warned_commit_failure_ = true;
            }
            return nullptr;
        }
        return &info;
    }

    void release_frame(frame_ticket_t& ticket)
    {
        if (ticket.selection() != media::prepared_frame_selection_e::new_frame || ticket.frame() == nullptr ||
            ticket.frame()->readiness() != media::source_frame_readiness_e::submitted) {
            return;
        }

        auto& info = ticket.frame()->value();
        if (info.stream) {
            info.stream->discard_exact(info.upload_version);
        }
        info.input_frame = nullptr;
        frame_queue_.cancel(ticket);
    }

    void reset_frames() { frame_queue_.reset(); }

    void start_async()
    {
        post_control([](callback_s& self) { self.start_capture_control(); });
    }

    void acknowledge_render_release()
    {
        auto expected = phase_e::release_requested;
        if (phase_.compare_exchange_strong(expected, phase_e::reconfiguring)) {
            post_control([](callback_s& self) { self.reconfigure_capture_control(); });
        }
    }

    void stop_async()
    {
        terminal_phase_ = phase_e::stopped;
        stop_requested_ = true;

        auto phase = phase_.load();
        while (phase == phase_e::running || phase == phase_e::release_requested) {
            if (phase_.compare_exchange_weak(phase, phase_e::stopping)) {
                post_control([](callback_s& self) { self.retire_capture(phase_e::stopped); });
                return;
            }
        }
    }

    bool    requires_render_release() const { return phase_.load() == phase_e::release_requested; }
    phase_e phase() const { return phase_.load(); }

    metrics_s metrics() const
    {
        decklink_ptr<input_video_buffer_allocator_s> allocator;
        {
            const std::scoped_lock lock(upload_mutex_);
            allocator = allocator_;
        }
        return {
            .frames_received            = frames_received_.load(),
            .frames_missing             = frames_missing_.load(),
            .no_input_source_frames     = no_input_source_frames_.load(),
            .upload_slot_drops          = upload_slot_drops_.load(),
            .upload_acquire_slow_count  = allocator ? allocator->upload_acquire_slow_count() : 0,
            .upload_acquire_failures    = allocator ? allocator->upload_acquire_failures() : 0,
            .upload_acquire_wait_max_us = allocator ? allocator->upload_acquire_wait_max_us() : 0,
            .content_frames_sampled     = content_frames_sampled_.load(),
            .content_frame_repeats      = content_frame_repeats_.load(),
            .content_repeat_streak      = content_repeat_streak_.load(),
            .content_repeat_streak_max  = content_repeat_streak_max_.load(),
            .available_video_frames     = available_video_frames_.load(),
            .source_queue               = frame_queue_.metrics(),
        };
    }

    // ── IUnknown ─────────────────────────────────────────────────────────────

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) noexcept final
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;

        if (decklink_iid_matches<IUnknown>(iid) || decklink_iid_matches<IDeckLinkInputCallback>(iid)) {
            *ppv = static_cast<IDeckLinkInputCallback*>(this);
            AddRef();
            return S_OK;
        }
        if (decklink_iid_matches<IDeckLinkVideoBufferAllocatorProvider>(iid)) {
            *ppv = static_cast<IDeckLinkVideoBufferAllocatorProvider*>(this);
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

} // namespace

namespace miximus::nodes::decklink::detail {

class input_capture_s::impl_s
{
  public:
    decklink_ptr<callback_s>                  callback;
    std::optional<callback_s::frame_ticket_t> prepared_frame;
};

input_capture_s::input_capture_s(gpu::transfer::texture_upload_service_s*              upload_service,
                                 utils::serial_executor_s*                             control_executor,
                                 decklink_ptr<IDeckLinkInput>                          device,
                                 std::shared_ptr<device_reservation_s<IDeckLinkInput>> reservation,
                                 std::string                                           device_name)
    : impl_(std::make_unique<impl_s>())
{
    impl_->callback = make_decklink_ptr<callback_s>(
        upload_service, control_executor, std::move(device), std::move(reservation), std::move(device_name));
}

input_capture_s::~input_capture_s() { stop_async(); }

void input_capture_s::start_async() { impl_->callback->start_async(); }

void input_capture_s::stop_async()
{
    if (impl_->callback) {
        impl_->callback->stop_async();
    }
}

void input_capture_s::acknowledge_render_release() { impl_->callback->acknowledge_render_release(); }

void input_capture_s::advance_frames(utils::flicks program_pts, utils::flicks target_time, bool discontinuity)
{
    impl_->callback->advance_frames(program_pts, target_time, discontinuity);
}

bool input_capture_s::submit_frame(utils::flicks program_pts, utils::flicks early_tolerance)
{
    impl_->prepared_frame.reset();
    if (!impl_->callback || impl_->callback->phase() != phase_e::running) {
        return false;
    }

    impl_->prepared_frame.emplace(impl_->callback->select_frame(program_pts, early_tolerance));
    if (!impl_->callback->submit_frame(*impl_->prepared_frame)) {
        impl_->prepared_frame.reset();
        return false;
    }
    return true;
}

std::optional<captured_input_frame_s> input_capture_s::resolve_frame()
{
    auto* frame = impl_->callback && impl_->prepared_frame.has_value()
                      ? impl_->callback->resolve_frame(*impl_->prepared_frame)
                      : nullptr;
    if (frame == nullptr || !frame->frame) {
        return std::nullopt;
    }
    return captured_input_frame_s{
        .frame      = frame->frame,
        .dimensions = frame->src_dim,
        .colorspace = frame->colorspace,
    };
}

void input_capture_s::release_prepared_frame()
{
    if (impl_->callback && impl_->prepared_frame.has_value()) {
        impl_->callback->release_frame(*impl_->prepared_frame);
    }
    impl_->prepared_frame.reset();
}

void input_capture_s::reset_frames()
{
    release_prepared_frame();
    if (impl_->callback) {
        impl_->callback->reset_frames();
    }
}

bool input_capture_s::requires_render_release() const { return impl_->callback->requires_render_release(); }

input_capture_s::phase_e input_capture_s::phase() const { return impl_->callback->phase(); }

input_capture_s::metrics_s input_capture_s::metrics() const
{
    const auto metrics = impl_->callback->metrics();
    return {
        .frames_received            = metrics.frames_received,
        .frames_missing             = metrics.frames_missing,
        .no_input_source_frames     = metrics.no_input_source_frames,
        .upload_slot_drops          = metrics.upload_slot_drops,
        .upload_acquire_slow_count  = metrics.upload_acquire_slow_count,
        .upload_acquire_failures    = metrics.upload_acquire_failures,
        .upload_acquire_wait_max_us = metrics.upload_acquire_wait_max_us,
        .content_frames_sampled     = metrics.content_frames_sampled,
        .content_frame_repeats      = metrics.content_frame_repeats,
        .content_repeat_streak      = metrics.content_repeat_streak,
        .content_repeat_streak_max  = metrics.content_repeat_streak_max,
        .available_video_frames     = metrics.available_video_frames,
        .source_queue               = metrics.source_queue,
    };
}

} // namespace miximus::nodes::decklink::detail
