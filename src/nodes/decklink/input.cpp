#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/allocator.hpp"
#include "detail/colorspace.hpp"
#include "detail/platform_compat.hpp"
#include "gpu/color_transfer.hpp"
#include "gpu/context.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "gpu/textured_quad.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
#include "utils/flicks.hpp"
#include "utils/observed_value.hpp"
#include "utils/serial_executor.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_set>
#include <utility>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace miximus::nodes::decklink;
using namespace miximus::nodes::decklink::detail;
auto log() { return getlog("decklink"); }

template <typename T>
nlohmann::json status_value(const std::optional<T>& value)
{
    return value.has_value() ? nlohmann::json(*value) : nlohmann::json(nullptr);
}

void write_device_status(core::node_status_registry_s::writer_s& writer, const device_status_s& status)
{
    writer.write("signal_locked", status_value(status.input_signal_locked));
    writer.write("ancillary_signal_locked", status_value(status.ancillary_signal_locked));
    writer.write("capture_busy", status_value(status.capture_busy));
    writer.write("pcie_link_width", status_value(status.pcie_link_width));
    writer.write("pcie_link_speed", status_value(status.pcie_link_speed));
    writer.write("temperature_c", status_value(status.temperature_c));
    writer.write("active_format", status_value(status.current_input_mode));
    writer.write("detected_format", status_value(status.detected_input_mode));
    writer.write("detected_colorspace", status_value(status.detected_colorspace));
    writer.write("detected_dynamic_range", status_value(status.detected_dynamic_range));
    writer.write("detected_field_dominance", status_value(status.detected_field_dominance));
    writer.write("detected_sdi_link_configuration", status_value(status.detected_sdi_link_configuration));
    writer.write("input_pixel_format", status_value(status.current_input_pixel_format));
}

// Metadata for a UYUV texture uploaded by the shared transfer worker.
struct frame_info_s
{
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
    gpu::texture_s*                                         texture{};
    gpu::vec2i_t                                            src_dim{};
    BMDColorspace                                           colorspace{bmdColorspaceRec709};
};

class device_reservation_s
{
    IDeckLinkInput* device_;

    static auto& mutex()
    {
        static std::mutex value;
        return value;
    }

    static auto& devices()
    {
        static std::unordered_set<IDeckLinkInput*> value;
        return value;
    }

    explicit device_reservation_s(IDeckLinkInput* device)
        : device_(device)
    {
    }

  public:
    device_reservation_s(const device_reservation_s&)            = delete;
    device_reservation_s& operator=(const device_reservation_s&) = delete;
    device_reservation_s(device_reservation_s&&)                 = delete;
    device_reservation_s& operator=(device_reservation_s&&)      = delete;

    ~device_reservation_s()
    {
        const std::scoped_lock lock(mutex());
        devices().erase(device_);
    }

    static std::shared_ptr<device_reservation_s> acquire(IDeckLinkInput* device)
    {
        const std::scoped_lock lock(mutex());
        if (device == nullptr || !devices().emplace(device).second) {
            return {};
        }
        return std::shared_ptr<device_reservation_s>(new device_reservation_s(device));
    }
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
    enum class phase_e : uint8_t
    {
        starting,
        running,
        release_requested,
        reconfiguring,
        stopping,
        stopped,
        failed,
    };

  private:
    std::atomic_ulong ref_count_{1};

    struct upload_metadata_s
    {
        utils::flicks arrival_time;
        gpu::vec2i_t  src_dim{};
        BMDColorspace colorspace{bmdColorspaceRec709};
    };

    gpu::transfer::texture_upload_service_s*                upload_service_;
    utils::serial_executor_s*                               control_executor_;
    decklink_ptr<IDeckLinkInput>                            device_;
    std::shared_ptr<device_reservation_s>                   reservation_;
    std::string                                             device_name_;
    decklink_ptr<allocator_s>                               allocator_;
    std::mutex                                              upload_mutex_;
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream_;
    std::map<uint64_t, upload_metadata_s>                   upload_metadata_;

    std::atomic<BMDDisplayMode> pending_display_mode_{bmdModeUnknown};
    std::atomic<BMDColorspace>  colorspace_{bmdColorspaceRec709};
    std::atomic_bool            stop_requested_{false};
    std::atomic<phase_e>        phase_{phase_e::starting};
    std::atomic<BMDDisplayMode> active_display_mode_{bmdModeUnknown};
    bool                        capture_active_{};

    std::atomic_uint64_t                  frames_received_{0};
    std::atomic_uint64_t                  frames_missing_{0};
    std::atomic_uint64_t                  no_input_source_frames_{0};
    std::atomic_uint64_t                  upload_slot_drops_{0};
    std::atomic_uint32_t                  available_video_frames_{0};
    std::chrono::steady_clock::time_point next_queue_poll_;
    BMDTimeValue                          last_stream_time_{};
    bool                                  has_stream_time_{};
    std::atomic_bool                      reset_stream_time_;
    std::atomic_bool                      warned_missing_custom_buffer_;

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
                self->stop_requested_ = true;
                self->retire_capture(phase_e::failed);
            } catch (...) {
                log()->error("DeckLink input control task failed");
                self->stop_requested_ = true;
                self->retire_capture(phase_e::failed);
            }
        });
        if (!accepted) {
            log()->error("DeckLink input control executor rejected a task during shutdown");
            phase_ = phase_e::failed;
        }
    }

    void release_upload_pool()
    {
        decklink_ptr<allocator_s> allocator;
        {
            const std::scoped_lock lock(upload_mutex_);
            upload_metadata_.clear();
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
        phase_               = phase_e::running;
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
        }
    }

    void reconfigure_capture_control()
    {
        if (stop_requested_.load()) {
            retire_capture(phase_e::stopped);
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
    struct metrics_s
    {
        uint64_t frames_received{};
        uint64_t frames_missing{};
        uint64_t no_input_source_frames{};
        uint64_t upload_slot_drops{};
        uint32_t available_video_frames{};
    };

    callback_s(gpu::transfer::texture_upload_service_s* upload_service,
               utils::serial_executor_s*                control_executor,
               decklink_ptr<IDeckLinkInput>             device,
               std::shared_ptr<device_reservation_s>    reservation,
               std::string                              device_name)
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
                                                      IDeckLinkVideoBufferAllocator** outAllocator) final
    {
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

        try {
            const gpu::transfer::texture_transfer_requirements_s requirements{
                .dimensions        = {static_cast<int>(rowBytes / 4), static_cast<int>(height)},
                .format            = gpu::texture_s::format_e::uyuv_u10,
                .row_stride        = static_cast<size_t>(rowBytes),
                .byte_size         = bufferSize,
                .address_alignment = 64,
                .host_access       = gpu::transfer::host_access_e::overwrite,
            };
            const auto stream = upload_service_->create_stream({
                .requirements      = requirements,
                .max_slots         = 6,
                .generate_mip_maps = false,
            });

            auto allocator = make_decklink_ptr<allocator_s>(bufferSize);
            allocator->set_upload_stream(stream);
            {
                const std::scoped_lock lock(upload_mutex_);
                if (allocator_) {
                    log()->error("DeckLink requested a new allocator before the previous pool was released");
                    return E_FAIL;
                }
                upload_metadata_.clear();
                upload_stream_ = stream;
                allocator_     = std::move(allocator);
            }
            allocator_->AddRef();
            *outAllocator = allocator_.get();
            return S_OK;
        } catch (const std::exception& error) {
            log()->error("Failed to create DeckLink input transfer stream: {}", error.what());
        } catch (...) {
            log()->error("Failed to create DeckLink input transfer stream");
        }
        return E_OUTOFMEMORY;
    }

    // ── IDeckLinkInputCallback ───────────────────────────────────────────────

    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame,
                                                     IDeckLinkAudioInputPacket* /*audioPacket*/) final
    {
        if (phase_.load() != phase_e::running) {
            return S_OK;
        }

        try {
            return video_input_frame_arrived(videoFrame);
        } catch (const std::exception& error) {
            log()->error("DeckLink input frame callback failed: {}", error.what());
        } catch (...) {
            log()->error("DeckLink input frame callback failed");
        }
        if (!stop_requested_.exchange(true)) {
            phase_ = phase_e::stopping;
            post_control([](callback_s& self) { self.retire_capture(phase_e::failed); });
        }
        return E_FAIL;
    }

  private:
    HRESULT video_input_frame_arrived(IDeckLinkVideoInputFrame* videoFrame)
    {
        const auto frame_arrival_time = utils::flicks_now();

        if (videoFrame == nullptr) {
            return S_OK;
        }

        ++frames_received_;
        if ((videoFrame->GetFlags() & bmdFrameHasNoInputSource) != 0) {
            ++no_input_source_frames_;
        }

        constexpr BMDTimeScale metric_time_scale = 1'000'000;
        BMDTimeValue           stream_time{};
        BMDTimeValue           frame_duration{};
        if (reset_stream_time_.exchange(false)) {
            has_stream_time_ = false;
        }
        if (videoFrame->GetStreamTime(&stream_time, &frame_duration, metric_time_scale) == S_OK && frame_duration > 0) {
            if (has_stream_time_ && stream_time > last_stream_time_ + frame_duration) {
                const auto missing = ((stream_time - last_stream_time_) / frame_duration) - 1;
                if (missing < 100) {
                    frames_missing_.fetch_add(static_cast<uint64_t>(missing));
                }
            }
            last_stream_time_ = stream_time;
            has_stream_time_  = true;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_queue_poll_) {
            uint32_t available{};
            if (device_->GetAvailableVideoFrameCount(&available) == S_OK) {
                available_video_frames_ = available;
            }
            next_queue_poll_ = now + std::chrono::seconds(1);
        }

        const gpu::vec2i_t src_dim{videoFrame->GetWidth(), videoFrame->GetHeight()};
        auto               video_buffer  = query_decklink_interface<IDeckLinkVideoBuffer>(videoFrame);
        auto               upload_buffer = query_upload_video_buffer(video_buffer.get());

        allocator_s* allocator{};
        {
            const std::scoped_lock lock(upload_mutex_);
            allocator = allocator_.get();
        }

        if (!upload_buffer || upload_buffer->allocator() != allocator) {
            ++upload_slot_drops_;
            if (!upload_buffer && !warned_missing_custom_buffer_.exchange(true)) {
                log()->warn("DeckLink returned a frame that was not allocated by its active custom allocator");
            }
            return S_OK;
        }

        const auto version = upload_buffer->upload_version();
        if (version == 0) {
            ++upload_slot_drops_;
            log()->warn("VideoInputFrameArrived dropped frame: no upload slot");
            return S_OK;
        }

        {
            const std::scoped_lock lock(upload_mutex_);
            upload_metadata_.insert_or_assign(version,
                                              upload_metadata_s{
                                                  .arrival_time = frame_arrival_time,
                                                  .src_dim      = src_dim,
                                                  .colorspace   = get_frame_colorspace(videoFrame),
                                              });
        }
        upload_buffer->submit_upload();
        return S_OK;
    }

  public:
    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                                                      IDeckLinkDisplayMode*            newDisplayMode,
                                                      BMDDetectedVideoInputFormatFlags /*detectedSignalFlags*/) final
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

    std::optional<frame_info_s> get_rendered_frame(utils::flicks pts, utils::flicks flush)
    {
        const std::scoped_lock lock(upload_mutex_);
        if (!upload_stream_) {
            return std::nullopt;
        }

        const auto latest_ready = upload_stream_->latest_ready_version();
        auto       selected     = upload_metadata_.end();
        for (auto it = upload_metadata_.begin(); it != upload_metadata_.end() && it->first <= latest_ready; ++it) {
            if (it->second.arrival_time <= pts) {
                selected = it;
            }
        }
        if (selected == upload_metadata_.end()) {
            std::erase_if(upload_metadata_, [flush](const auto& entry) { return entry.second.arrival_time < flush; });
            return std::nullopt;
        }

        const auto version  = selected->first;
        auto       metadata = selected->second;
        auto*      texture  = upload_stream_->consume_through(version);
        if (texture == nullptr || upload_stream_->current_version() != version) {
            return std::nullopt;
        }

        for (auto it = upload_metadata_.begin(); it != upload_metadata_.end();) {
            if (it->first <= version || it->second.arrival_time < flush) {
                it = upload_metadata_.erase(it);
            } else {
                ++it;
            }
        }
        return frame_info_s{
            .stream     = upload_stream_,
            .texture    = texture,
            .src_dim    = metadata.src_dim,
            .colorspace = metadata.colorspace,
        };
    }

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
        if (!stop_requested_.exchange(true)) {
            phase_ = phase_e::stopping;
            post_control([](callback_s& self) { self.retire_capture(phase_e::stopped); });
        }
    }

    bool    requires_render_release() const { return phase_.load() == phase_e::release_requested; }
    phase_e phase() const { return phase_.load(); }

    metrics_s metrics() const
    {
        return {
            .frames_received        = frames_received_.load(),
            .frames_missing         = frames_missing_.load(),
            .no_input_source_frames = no_input_source_frames_.load(),
            .upload_slot_drops      = upload_slot_drops_.load(),
            .available_video_frames = available_video_frames_.load(),
        };
    }

    // ── IUnknown ─────────────────────────────────────────────────────────────

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) final
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

    ULONG STDMETHODCALLTYPE AddRef() final { return ++ref_count_; }
    ULONG STDMETHODCALLTYPE Release() final
    {
        const ULONG count = --ref_count_;
        if (count == 0) {
            delete this;
        }
        return count;
    }
};

// ─── node_impl ───────────────────────────────────────────────────────────────

class node_impl : public node_i
{
    decklink_ptr<callback_s> callback_;

    std::unique_ptr<gpu::framebuffer_s>                       framebuffer_;
    std::unique_ptr<gpu::textured_quad_s>                     textured_quad_;
    std::optional<frame_info_s>                               work_frame_;
    utils::observed_value_s<uint64_t>                         device_version_;
    utils::observed_value_s<std::pair<std::string, bool>>     capture_selection_;
    utils::observed_value_s<BMDColorspace>                    colorspace_;
    utils::observed_value_s<std::pair<std::string, uint64_t>> device_status_version_;
    std::chrono::steady_clock::time_point                     next_metrics_status_;
    gpu::color_conversion_s                                   yuv_conversion_{};
    gpu::mat3                                                 gamut_conversion_{1.0F};

    output_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    void stop_capture()
    {
        work_frame_.reset();
        framebuffer_.reset();
        if (callback_) {
            callback_->stop_async();
            callback_ = nullptr;
        }
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

    void publish_metrics(core::node_status_registry_s* status_registry)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!callback_ || now < next_metrics_status_) {
            return;
        }

        const auto metrics = callback_->metrics();
        auto       writer  = status_registry->write_node(id_);
        writer.write("frames_received", metrics.frames_received);
        writer.write("frames_missing", metrics.frames_missing);
        writer.write("no_input_source_frames", metrics.no_input_source_frames);
        writer.write("upload_slot_drops", metrics.upload_slot_drops);
        writer.write("available_video_frames", metrics.available_video_frames);
        next_metrics_status_ = now + std::chrono::seconds(1);
    }

    void prepare_active_capture(core::app_state_s* app, core::node_status_registry_s* status_registry)
    {
        if (!callback_) {
            return;
        }

        if (callback_->requires_render_release()) {
            work_frame_.reset();
            callback_->acknowledge_render_release();
        }

        const auto phase = callback_->phase();
        if (phase == callback_s::phase_e::failed) {
            log()->error("DeckLink input capture failed");
            stop_capture();
            capture_selection_.reset();
            status_registry->write(id_, "connected", false);
            return;
        }
        if (phase == callback_s::phase_e::stopped) {
            callback_ = nullptr;
            capture_selection_.reset();
            status_registry->write(id_, "connected", false);
            return;
        }

        if (phase == callback_s::phase_e::running) {
            const auto pts   = app->frame_info.timestamp;
            const auto flush = pts - app->frame_info.duration * 2;
            work_frame_      = callback_->get_rendered_frame(pts, flush);
        } else {
            work_frame_.reset();
        }
    }

    bool start_capture(core::app_state_s* app, decklink_ptr<IDeckLinkInput> device, std::string_view device_name)
    {
        auto reservation = device_reservation_s::acquire(device.get());
        if (!reservation) {
            return false;
        }

        log()->info("Scheduling DeckLink input setup for {}", device_name);
        callback_ = make_decklink_ptr<callback_s>(app->texture_upload_service(),
                                                  app->decklink_registry()->input_control_executor(),
                                                  std::move(device),
                                                  std::move(reservation),
                                                  std::string(device_name));
        callback_->start_async();
        return true;
    }

  public:
    explicit node_impl() = default;

    ~node_impl() override { stop_capture(); }

    node_impl(const node_impl&)      = delete;
    node_impl(node_impl&&)           = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* /*traits*/) final
    {
        auto* sr = app->status_registry();

        const auto current_version     = app->decklink_registry()->get_device_list_version();
        const bool device_list_changed = device_version_.observe(current_version);
        if (device_list_changed) {
            sr->write(id_, "device_names", app->decklink_registry()->get_input_options());
        }

        prepare_active_capture(app, sr);

        auto device_name = state.get_option<std::string>("device_name");
        auto enabled     = state.get_option<bool>("enabled");

        if (device_list_changed && callback_ && !app->decklink_registry()->get_input(device_name)) {
            stop_capture();
            capture_selection_.reset();
        }

        publish_device_status(app, device_name);
        publish_metrics(sr);

        const auto selection = std::pair(device_name, enabled);
        if (capture_selection_.would_change(selection)) {
            stop_capture();

            if (!enabled) {
                capture_selection_.commit(selection);
                sr->write(id_, "connected", false);
                return;
            }

            auto device = app->decklink_registry()->get_input(device_name);
            if (!device) {
                sr->write(id_, "connected", false);
                return;
            }

            if (!start_capture(app, std::move(device), device_name)) {
                sr->write(id_, "connected", false);
                return;
            }
            capture_selection_.commit(selection);
        }

        sr->write(id_, "connected", callback_ && callback_->phase() == callback_s::phase_e::running);
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        if (!work_frame_ || work_frame_->texture == nullptr) {
            iface_tex_.set_value(framebuffer_ ? framebuffer_->texture() : nullptr);
            return;
        }

        if (!textured_quad_) {
            auto shader    = app->ctx()->get_shader(gpu::shader_program_s::name_e::yuv_to_rgb);
            textured_quad_ = std::make_unique<gpu::textured_quad_s>(shader);
        }

        const auto src_dim = work_frame_->src_dim;

        if (!framebuffer_ || framebuffer_->texture()->texture_dimensions() != src_dim) {
            framebuffer_ = std::make_unique<gpu::framebuffer_s>(src_dim, gpu::texture_s::format_e::rgb_f16);
        }

        auto shader = textured_quad_->shader();
        shader->set_uniform("target_width", src_dim.x);

        if (colorspace_.observe(work_frame_->colorspace)) {
            const auto transfer = get_color_transfer(colorspace_.value());
            yuv_conversion_     = gpu::get_color_transfer_from_yuv(transfer);
            gamut_conversion_   = gpu::get_gamut_transfer_to_rec709(transfer);
        }

        shader->set_uniform("transfer", yuv_conversion_.matrix);
        shader->set_uniform("transfer_offset", yuv_conversion_.offset);
        shader->set_uniform("gamut_transfer", gamut_conversion_);

        framebuffer_->begin_render(gpu::framebuffer_s::load_op_e::clear);
        textured_quad_->draw(work_frame_->texture);
        gpu::framebuffer_s::end_render();

        auto fb_tex = framebuffer_->texture();
        fb_tex->generate_mip_maps();
        iface_tex_.set_value(fb_tex);
    }

    void complete(core::app_state_s* /*app*/) final { work_frame_.reset(); }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",    "DeckLink input"},
            {"enabled", true            },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "device_name") {
            return normalize_option_value<std::string_view>(value);
        }
        if (name == "enabled") {
            return normalize_option_value<bool>(value);
        }
        return option_result_e::invalid;
    }

    std::string_view type() const final { return "decklink_input"; }
};
} // namespace

namespace miximus::nodes::decklink {
std::shared_ptr<node_i> create_input_node() { return std::make_shared<node_impl>(); }
} // namespace miximus::nodes::decklink
