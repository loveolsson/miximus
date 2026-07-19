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
#include "utils/observed_value.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    std::atomic_ulong ref_count_{1};

    struct upload_metadata_s
    {
        utils::flicks arrival_time;
        gpu::vec2i_t  src_dim{};
        BMDColorspace colorspace{bmdColorspaceRec709};
    };

    gpu::transfer::texture_upload_service_s*                upload_service_;
    decklink_ptr<IDeckLinkInput>                            device_;
    decklink_ptr<allocator_s>                               allocator_;
    std::mutex                                              upload_mutex_;
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream_;
    std::map<uint64_t, upload_metadata_s>                   upload_metadata_;

    std::atomic<BMDDisplayMode> new_display_mode_{bmdModeUnknown};
    std::atomic<BMDColorspace>  colorspace_{bmdColorspaceRec709};
    std::atomic_bool            stopping_{false};
    std::atomic_bool            failed_{false};

    std::atomic_uint64_t                  frames_received_{0};
    std::atomic_uint64_t                  frames_missing_{0};
    std::atomic_uint64_t                  no_input_source_frames_{0};
    std::atomic_uint64_t                  upload_slot_drops_{0};
    std::atomic_uint32_t                  available_video_frames_{0};
    std::chrono::steady_clock::time_point next_queue_poll_;
    BMDTimeValue                          last_stream_time_{};
    bool                                  has_stream_time_{};
    std::atomic_bool                      reset_stream_time_;

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

    static auto copy_fallback_frame(IDeckLinkVideoBuffer*                                          video_buffer,
                                    const std::shared_ptr<gpu::transfer::texture_upload_stream_s>& stream,
                                    size_t frame_size) -> std::optional<gpu::transfer::texture_upload_lease_s>
    {
        if (video_buffer == nullptr || !stream) {
            return std::nullopt;
        }
        auto upload = stream->try_acquire();
        if (!upload || video_buffer->StartAccess(bmdBufferAccessRead) != S_OK) {
            return std::nullopt;
        }

        void*      src_data = nullptr;
        const bool copied   = video_buffer->GetBytes(&src_data) == S_OK && src_data != nullptr;
        if (copied) {
            std::memcpy(upload->ptr(), src_data, std::min(frame_size, upload->size()));
        }
        video_buffer->EndAccess(bmdBufferAccessRead);
        return copied ? std::move(upload) : std::nullopt;
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

    callback_s(gpu::transfer::texture_upload_service_s* upload_service, decklink_ptr<IDeckLinkInput> device)
        : upload_service_(upload_service)
        , device_(std::move(device))
        , allocator_(make_decklink_ptr<allocator_s>())
    {
    }

    ~callback_s() override
    {
        allocator_->destroy_free_buffers();
        const std::scoped_lock lock(upload_mutex_);
        upload_metadata_.clear();
        upload_stream_.reset();
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

        if (stopping_.load()) {
            return E_FAIL;
        }

        // Only hand over our allocator for the primary capture format.
        // Conversion frames (different pixel format) use DeckLink's default.
        if (pixelFormat != bmdFormat10BitYUV) {
            return E_NOTIMPL;
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
            auto stream = upload_service_->create_stream({
                .requirements      = requirements,
                .max_slots         = 6,
                .generate_mip_maps = false,
            });
            {
                const std::scoped_lock lock(upload_mutex_);
                upload_metadata_.clear();
                upload_stream_ = stream;
            }
            allocator_->set_upload_stream(std::move(stream));
            allocator_->AddRef();
            *outAllocator = allocator_.get();
            return S_OK;
        } catch (const std::exception& error) {
            log()->error("Failed to create DeckLink input transfer stream: {}", error.what());
        } catch (...) {
            log()->error("Failed to create DeckLink input transfer stream");
        }
        failed_ = true;
        return E_OUTOFMEMORY;
    }

    // ── IDeckLinkInputCallback ───────────────────────────────────────────────

    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame,
                                                     IDeckLinkAudioInputPacket* /*audioPacket*/) final
    {
        if (stopping_.load()) {
            return S_OK;
        }

        try {
            return video_input_frame_arrived(videoFrame);
        } catch (const std::exception& error) {
            log()->error("DeckLink input frame callback failed: {}", error.what());
        } catch (...) {
            log()->error("DeckLink input frame callback failed");
        }
        failed_ = true;
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

        const auto         row_bytes = static_cast<size_t>(videoFrame->GetRowBytes());
        const gpu::vec2i_t src_dim{videoFrame->GetWidth(), videoFrame->GetHeight()};
        const auto         frame_size = row_bytes * static_cast<size_t>(src_dim.y);

        std::optional<gpu::transfer::texture_upload_lease_s> fallback_upload;
        uint64_t                                             version{};
        auto video_buffer = query_decklink_interface<IDeckLinkVideoBuffer>(videoFrame);
        if (video_buffer) {
            version = allocator_->upload_version(video_buffer.get());
        }

        std::shared_ptr<gpu::transfer::texture_upload_stream_s> stream;
        {
            const std::scoped_lock lock(upload_mutex_);
            stream = upload_stream_;
        }
        const bool custom_buffer = version != 0;
        if (!custom_buffer) {
            fallback_upload = copy_fallback_frame(video_buffer.get(), stream, frame_size);
            version         = fallback_upload ? fallback_upload->version() : 0;
        }

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
        if (custom_buffer) {
            if (!allocator_->submit_upload(video_buffer.get())) {
                ++upload_slot_drops_;
                const std::scoped_lock lock(upload_mutex_);
                upload_metadata_.erase(version);
            }
        } else if (fallback_upload) {
            fallback_upload->submit();
        }
        return S_OK;
    }

  public:
    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                                                      IDeckLinkDisplayMode*            newDisplayMode,
                                                      BMDDetectedVideoInputFormatFlags /*detectedSignalFlags*/) final
    {
        if (stopping_.load()) {
            return S_OK;
        }
        if (newDisplayMode == nullptr) {
            return E_INVALIDARG;
        }

        const bool display_mode_changed = (notificationEvents & bmdVideoInputDisplayModeChanged) != 0;
        const bool colorspace_changed   = (notificationEvents & bmdVideoInputColorspaceChanged) != 0;

        if (display_mode_changed) {
            new_display_mode_  = newDisplayMode->GetDisplayMode();
            reset_stream_time_ = true;
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
            return std::nullopt;
        }

        const auto version  = selected->first;
        const auto metadata = selected->second;
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

    BMDDisplayMode get_new_display_mode() { return new_display_mode_.exchange(bmdModeUnknown); }

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

    void begin_shutdown() { stopping_ = true; }
    bool failed() const { return failed_.load(); }

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
    decklink_ptr<IDeckLinkInput> device_;
    decklink_ptr<callback_s>     callback_;

    std::unique_ptr<gpu::framebuffer_s>                       framebuffer_;
    std::unique_ptr<gpu::textured_quad_s>                     textured_quad_;
    std::optional<frame_info_s>                               work_frame_;
    utils::observed_value_s<uint64_t>                         device_version_;
    utils::observed_value_s<BMDColorspace>                    colorspace_;
    utils::observed_value_s<std::pair<std::string, uint64_t>> device_status_version_;
    std::chrono::steady_clock::time_point                     next_metrics_status_;
    gpu::color_conversion_s                                   yuv_conversion_{};
    gpu::mat3                                                 gamut_conversion_{1.0F};

    output_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    static auto& devices_in_use()
    {
        static std::unordered_set<IDeckLinkInput*> devices;
        return devices;
    }

    void free_device()
    {
        devices_in_use().erase(device_.get());

        if (callback_) {
            callback_->begin_shutdown();
        }

        (void)device_->StopStreams();
        (void)device_->SetCallback(nullptr);
        (void)device_->FlushStreams();
        (void)device_->DisableAudioInput();
        (void)device_->DisableVideoInput();

        device_   = nullptr;
        callback_ = nullptr;
        framebuffer_.reset();
        work_frame_.reset();
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

    bool prepare_active_capture(core::app_state_s* app, core::node_status_registry_s* status_registry)
    {
        if (!device_ || !callback_) {
            return true;
        }
        if (callback_->failed()) {
            log()->error("DeckLink input callback failed; restarting the device");
            free_device();
            status_registry->write(id_, "connected", false);
            return false;
        }

        const auto pts   = app->frame_info.timestamp;
        const auto flush = pts - app->frame_info.duration * 2;
        work_frame_      = callback_->get_rendered_frame(pts, flush);

        const auto new_mode = callback_->get_new_display_mode();
        if (new_mode == bmdModeUnknown) {
            return true;
        }

        const auto stopped = device_->StopStreams();
        const auto enabled = device_->EnableVideoInputWithAllocatorProvider(
            new_mode, bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection, callback_.get());
        const auto started = enabled == S_OK ? device_->StartStreams() : E_FAIL;
        if (stopped == S_OK && enabled == S_OK && started == S_OK) {
            return true;
        }

        log()->error("Failed to restart DeckLink input after a format change");
        free_device();
        status_registry->write(id_, "connected", false);
        return false;
    }

    bool start_capture(core::app_state_s* app, decklink_ptr<IDeckLinkInput> device, std::string_view device_name)
    {
        log()->info("Setting up DeckLink input {}", device_name);
        auto callback = make_decklink_ptr<callback_s>(app->texture_upload_service(), device);

        decklink_ptr<IDeckLinkDisplayModeIterator> mode_iterator;
        decklink_ptr<IDeckLinkDisplayMode>         initial_mode;
        const auto iterator_result = device->GetDisplayModeIterator(mode_iterator.releaseAndGetAddressOf());
        const auto mode_result     = iterator_result == S_OK && mode_iterator
                                         ? mode_iterator->Next(initial_mode.releaseAndGetAddressOf())
                                         : E_FAIL;
        const auto callback_result = mode_result == S_OK && initial_mode ? device->SetCallback(callback.get()) : E_FAIL;
        const auto enable_result =
            callback_result == S_OK ? device->EnableVideoInputWithAllocatorProvider(initial_mode->GetDisplayMode(),
                                                                                    bmdFormat10BitYUV,
                                                                                    bmdVideoInputEnableFormatDetection,
                                                                                    callback.get())
                                    : E_FAIL;
        const auto start_result = enable_result == S_OK ? device->StartStreams() : E_FAIL;

        if (start_result != S_OK) {
            callback->begin_shutdown();
            (void)device->StopStreams();
            (void)device->SetCallback(nullptr);
            (void)device->DisableVideoInput();
            log()->error("Failed to start DeckLink input {}", device_name);
            return false;
        }

        device_   = std::move(device);
        callback_ = std::move(callback);
        devices_in_use().emplace(device_.get());
        return true;
    }

  public:
    explicit node_impl() = default;

    ~node_impl() override
    {
        if (device_) {
            free_device();
        }
    }

    node_impl(const node_impl&)      = delete;
    node_impl(node_impl&&)           = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* /*traits*/) final
    {
        auto* sr = app->status_registry();

        const auto current_version = app->decklink_registry()->get_device_list_version();
        if (device_version_.observe(current_version)) {
            sr->write(id_, "device_names", app->decklink_registry()->get_input_options());
        }

        if (!prepare_active_capture(app, sr)) {
            return;
        }

        auto device_name = state.get_option<std::string>("device_name");
        auto enabled     = state.get_option<bool>("enabled");

        publish_device_status(app, device_name);
        publish_metrics(sr);

        auto device = enabled ? app->decklink_registry()->get_input(device_name) : nullptr;
        if (device != device_) {
            if (device_) {
                free_device();
            }

            if (!device || devices_in_use().contains(device.get())) {
                sr->write(id_, "connected", false);
                return;
            }

            if (!start_capture(app, std::move(device), device_name)) {
                sr->write(id_, "connected", false);
                return;
            }
        }

        sr->write(id_, "connected", device_ != nullptr);
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
