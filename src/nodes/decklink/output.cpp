#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/colorspace.hpp"
#include "detail/platform_compat.hpp"
#include "gpu/color_transfer.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_download.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
#include "utils/observed_value.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace miximus::nodes::decklink;
using namespace std::chrono_literals;

class node_impl;

auto log() { return getlog("decklink"); }

struct mode_info_s
{
    BMDDisplayMode          mode;
    BMDTimeValue            frame_duration;
    BMDTimeScale            time_scale;
    gpu::vec2i_t            dim;
    gpu::color_conversion_s yuv_conversion;
    gpu::mat3               gamut_conversion;
    BMDColorspace           colorspace;
};

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
class callback_s
    : public IDeckLinkVideoOutputCallback
    , public IDeckLinkAudioOutputCallback
{
    std::atomic_ulong ref_count_{1};

    decklink_ptr<IDeckLinkOutput>                             device_;
    std::shared_ptr<gpu::transfer::texture_download_stream_s> download_stream_;
    decklink_ptr<IDeckLinkVideoFrame>                         last_frame_;

    mode_info_s             mode_info_;
    BMDTimeValue            pts_{0};
    std::atomic_bool        stopping_{false};
    std::atomic_bool        failed_{false};
    mutable std::mutex      stop_mutex_;
    std::condition_variable stop_condition_;
    bool                    playback_stopped_{};

    void set_colorspace_metadata(IDeckLinkMutableVideoFrame* frame) const
    {
        auto metadata = query_decklink_interface<IDeckLinkVideoFrameMutableMetadataExtensions>(frame);
        if (metadata) {
            (void)metadata->SetInt(bmdDeckLinkFrameMetadataColorspace, mode_info_.colorspace);
        }
    }

  public:
    callback_s(std::shared_ptr<gpu::transfer::texture_download_stream_s> download_stream,
               decklink_ptr<IDeckLinkOutput>                             device,
               mode_info_s                                               mode_info)
        : device_(std::move(device))
        , download_stream_(std::move(download_stream))
        , mode_info_(mode_info)
    {
    }

    bool initialize()
    {
        IDeckLinkMutableVideoFrame* frame = nullptr;

        if (device_->CreateVideoFrame(
                mode_info_.dim.x, mode_info_.dim.y, mode_info_.dim.x * 2, bmdFormat8BitYUV, 0, &frame) != S_OK) {
            return false;
        }
        decklink_ptr frame_owner(frame, false);
        set_colorspace_metadata(frame);

        auto buffer = query_decklink_interface<IDeckLinkVideoBuffer>(frame);
        if (!buffer || buffer->StartAccess(bmdBufferAccessWrite) != S_OK) {
            return false;
        }

        void* data = nullptr;
        if (buffer->GetBytes(&data) != S_OK || data == nullptr) {
            (void)buffer->EndAccess(bmdBufferAccessWrite);
            return false;
        }
        constexpr uint16_t uyvy_black = 0x1080;
        std::fill_n(static_cast<uint16_t*>(data), static_cast<size_t>(mode_info_.dim.x * mode_info_.dim.y), uyvy_black);
        if (buffer->EndAccess(bmdBufferAccessWrite) != S_OK) {
            return false;
        }

        for (int i = 0; i < 4; ++i) {
            if (device_->ScheduleVideoFrame(frame, pts_, mode_info_.frame_duration, mode_info_.time_scale) != S_OK) {
                return false;
            }
            pts_ += mode_info_.frame_duration;
        }
        last_frame_ = frame;
        return true;
    }

    ~callback_s() override = default;

    callback_s(callback_s&&)                 = delete;
    callback_s(const callback_s&)            = delete;
    callback_s& operator=(callback_s&&)      = delete;
    callback_s& operator=(const callback_s&) = delete;

    /**
     * IUnknown
     */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) final
    {
        if (ppv == nullptr) {
            return E_POINTER;
        }
        *ppv = nullptr;

        if (decklink_iid_matches<IUnknown>(iid) || decklink_iid_matches<IDeckLinkVideoOutputCallback>(iid)) {
            *ppv = static_cast<IDeckLinkVideoOutputCallback*>(this);
        } else if (decklink_iid_matches<IDeckLinkAudioOutputCallback>(iid)) {
            *ppv = static_cast<IDeckLinkAudioOutputCallback*>(this);
        } else {
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
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

    /**
     * IDeckLinkVideoOutputCallback
     */
    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* /*completedFrame*/,
                                                      BMDOutputFrameCompletionResult result) final
    {
        if (stopping_.load() || result == bmdOutputFrameFlushed) {
            return S_OK;
        }

        try {
            return scheduled_frame_completed(result);
        } catch (const std::exception& error) {
            log()->error("DeckLink output callback failed: {}", error.what());
        } catch (...) {
            log()->error("DeckLink output callback failed");
        }
        failed_ = true;
        return E_FAIL;
    }

  private:
    HRESULT scheduled_frame_completed(BMDOutputFrameCompletionResult result)
    {
        auto frame = download_stream_->try_consume_latest();
        if (frame) {
            decklink_ptr<IDeckLinkMutableVideoFrame> dst_frame;
            const int32_t                            row_bytes = ((mode_info_.dim.x + 47) / 48) * 128;

            if (SUCCEEDED(device_->CreateVideoFrame(mode_info_.dim.x,
                                                    mode_info_.dim.y,
                                                    row_bytes,
                                                    bmdFormat10BitYUV,
                                                    0,
                                                    dst_frame.releaseAndGetAddressOf()))) {
                set_colorspace_metadata(dst_frame.get());
                auto dst_buffer = dst_frame.query<IDeckLinkVideoBuffer>();
                if (dst_buffer && dst_buffer->StartAccess(bmdBufferAccessWrite) == S_OK) {
                    void*      dst_ptr = nullptr;
                    const auto size    = static_cast<size_t>(row_bytes) * static_cast<size_t>(mode_info_.dim.y);
                    if (dst_buffer->GetBytes(&dst_ptr) == S_OK && dst_ptr != nullptr && size == frame->size()) {
                        std::memcpy(dst_ptr, frame->ptr(), size);
                        if (dst_buffer->EndAccess(bmdBufferAccessWrite) == S_OK) {
                            last_frame_ = dst_frame.query<IDeckLinkVideoFrame>();
                        }
                    } else {
                        (void)dst_buffer->EndAccess(bmdBufferAccessWrite);
                    }
                }
            }
        }

        if (last_frame_) {
            if (result != bmdOutputFrameCompleted) {
                log()->warn("Frame dropped");
            }

            if (device_->ScheduleVideoFrame(
                    last_frame_.get(), pts_, mode_info_.frame_duration, mode_info_.time_scale) != S_OK) {
                failed_ = true;
                return E_FAIL;
            }
            pts_ += mode_info_.frame_duration;
        }

        return S_OK;
    }

  public:
    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() final
    {
        {
            const std::scoped_lock lock(stop_mutex_);
            playback_stopped_ = true;
        }
        stop_condition_.notify_all();
        return S_OK;
    }

    /**
     * IDeckLinkAudioOutputCallback
     */
    HRESULT STDMETHODCALLTYPE RenderAudioSamples(BOOL /*preroll*/) final { return S_OK; }

    void begin_shutdown() { stopping_ = true; }
    bool playback_stopped() const
    {
        const std::scoped_lock lock(stop_mutex_);
        return playback_stopped_;
    }
    bool wait_for_playback_stop(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(stop_mutex_);
        return stop_condition_.wait_for(lock, timeout, [this] { return playback_stopped_; });
    }
    bool failed() const { return failed_.load(); }
};
#pragma GCC diagnostic pop

class node_impl : public node_i
{
    struct pending_stop_s
    {
        decklink_ptr<IDeckLinkOutput>         device;
        decklink_ptr<callback_s>              callback;
        std::chrono::steady_clock::time_point deadline;
    };

    decklink_ptr<IDeckLinkOutput>                   device_;
    decklink_ptr<callback_s>                        callback_;
    std::optional<pending_stop_s>                   pending_stop_;
    std::map<std::string, mode_info_s, std::less<>> display_modes_;

    std::unique_ptr<gpu::framebuffer_s>                       framebuffer_scale_;
    std::unique_ptr<gpu::draw_state_s>                        draw_state_yuv_;
    std::unique_ptr<gpu::draw_state_s>                        draw_state_scale_;
    std::shared_ptr<gpu::transfer::texture_download_stream_s> download_stream_;
    utils::observed_value_s<std::string>                      display_mode_name_;
    mode_info_s*                                              display_mode_{};
    utils::observed_value_s<uint64_t>                         device_version_;

    input_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    static auto& devices_in_use()
    {
        static std::unordered_set<IDeckLinkOutput*> devices;
        return devices;
    }

    void finalize_pending_stop()
    {
        if (!pending_stop_) {
            return;
        }

        (void)pending_stop_->device->SetScheduledFrameCompletionCallback(nullptr);
        (void)pending_stop_->device->SetAudioCallback(nullptr);
        (void)pending_stop_->device->FlushBufferedAudioSamples();
        (void)pending_stop_->device->DisableAudioOutput();
        (void)pending_stop_->device->DisableVideoOutput();
        devices_in_use().erase(pending_stop_->device.get());
        pending_stop_.reset();
    }

    bool poll_pending_stop()
    {
        if (!pending_stop_) {
            return true;
        }
        if (!pending_stop_->callback->playback_stopped() &&
            std::chrono::steady_clock::now() < pending_stop_->deadline) {
            return false;
        }
        if (!pending_stop_->callback->playback_stopped()) {
            log()->warn("Timed out waiting for DeckLink playback to stop");
        }
        finalize_pending_stop();
        return true;
    }

    void free_device()
    {
        if (!device_) {
            return;
        }

        if (!callback_) {
            devices_in_use().erase(device_.get());
            (void)device_->SetScheduledFrameCompletionCallback(nullptr);
            (void)device_->DisableVideoOutput();
            device_ = nullptr;
            display_modes_.clear();
            display_mode_ = nullptr;
            return;
        }

        callback_->begin_shutdown();

        const auto stop_result = device_->StopScheduledPlayback(0, nullptr, 0);
        pending_stop_.emplace(pending_stop_s{
            .device   = std::move(device_),
            .callback = std::move(callback_),
            .deadline =
                stop_result == S_OK ? std::chrono::steady_clock::now() + 500ms : std::chrono::steady_clock::now(),
        });

        framebuffer_scale_.reset();
        download_stream_.reset();
        display_modes_.clear();
        display_mode_ = nullptr;
    }

    bool start_playback(core::app_state_s* app, std::string_view display_mode_name)
    {
        auto mode_it = display_modes_.find(display_mode_name);
        if (mode_it == display_modes_.end()) {
            return false;
        }

        auto* mode = &mode_it->second;
        bool  supported{};
        if (device_->DoesSupportVideoMode(bmdVideoConnectionUnspecified,
                                          mode->mode,
                                          bmdFormat10BitYUV,
                                          bmdNoVideoOutputConversion,
                                          bmdSupportedVideoModeDefault,
                                          nullptr,
                                          &supported) != S_OK ||
            !supported) {
            return false;
        }

        if (device_->EnableVideoOutput(mode->mode, bmdVideoOutputFlagDefault) != S_OK) {
            return false;
        }

        try {
            const int    row_pixels      = ((mode->dim.x + 47) / 48) * 32;
            const size_t frame_size      = static_cast<size_t>(row_pixels) * static_cast<size_t>(mode->dim.y) * 4;
            auto         download_stream = app->texture_download_service()->create_stream({
                        .dimensions = {row_pixels, mode->dim.y},
                        .format     = gpu::texture_s::format_e::uyuv_u10,
                        .byte_size  = frame_size,
                        .max_slots  = 7,
            });
            auto         callback        = make_decklink_ptr<callback_s>(download_stream, device_, *mode);

            if (device_->SetScheduledFrameCompletionCallback(callback.get()) != S_OK || !callback->initialize() ||
                device_->StartScheduledPlayback(0, mode->time_scale, 1.0) != S_OK) {
                callback->begin_shutdown();
                (void)device_->StopScheduledPlayback(0, nullptr, 0);
                (void)device_->SetScheduledFrameCompletionCallback(nullptr);
                (void)device_->DisableVideoOutput();
                return false;
            }

            display_mode_    = mode;
            download_stream_ = std::move(download_stream);
            callback_        = std::move(callback);
            return true;
        } catch (const std::exception& error) {
            log()->error("Failed to create DeckLink output resources: {}", error.what());
        } catch (...) {
            log()->error("Failed to create DeckLink output resources");
        }

        (void)device_->SetScheduledFrameCompletionCallback(nullptr);
        (void)device_->DisableVideoOutput();
        return false;
    }

  public:
    explicit node_impl() = default;

    ~node_impl() override
    {
        if (device_) {
            free_device();
        }
        if (pending_stop_) {
            (void)pending_stop_->callback->wait_for_playback_stop(500ms);
            finalize_pending_stop();
        }
    }

    node_impl(const node_impl&)      = delete;
    node_impl(node_impl&&)           = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&)      = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* traits) final
    {
        traits->must_run = true;
        auto* sr         = app->status_registry();

        // Rebuild device list only when the registry has changed
        const auto current_version = app->decklink_registry()->get_device_list_version();
        if (device_version_.observe(current_version)) {
            sr->write(id_, "device_names", nlohmann::json(app->decklink_registry()->get_output_names()));
        }

        auto device_name  = state.get_option<std::string>("device_name");
        auto display_mode = state.get_option<std::string>("display_mode");
        auto enabled      = state.get_option<bool>("enabled");

        if (!poll_pending_stop()) {
            sr->write(id_, "connected", false);
            return;
        }

        auto device = enabled ? app->decklink_registry()->get_output(device_name) : nullptr;
        if (device_ && (device != device_ || (callback_ && callback_->failed()))) {
            free_device();
            sr->write(id_, "connected", false);
            sr->write(id_, "display_modes", nlohmann::json::array());
            return;
        }

        const bool mode_changed = display_mode_name_.would_change(display_mode);
        if (device_ && callback_ && mode_changed) {
            display_mode_name_.commit(display_mode);
            free_device();
            sr->write(id_, "connected", false);
            return;
        }

        if (device_) {
            if (!callback_ && mode_changed) {
                display_mode_name_.commit(display_mode);
                if (!start_playback(app, display_mode)) {
                    log()->error("Failed to start DeckLink output {} with {}", device_name, display_mode);
                }
            }
            sr->write(id_, "connected", callback_ != nullptr);
            return;
        }

        sr->write(id_, "connected", false);
        auto& in_use = devices_in_use();
        if (!device || in_use.contains(device.get())) {
            return;
        }

        log()->info("Setting up DeckLink output {}", device_name);

        std::map<std::string, mode_info_s, std::less<>> display_modes;
        decklink_ptr<IDeckLinkDisplayModeIterator>      mode_iterator;
        if (device->GetDisplayModeIterator(mode_iterator.releaseAndGetAddressOf()) != S_OK || !mode_iterator) {
            log()->error("Failed to enumerate display modes for DeckLink output {}", device_name);
            return;
        }

        decklink_ptr<IDeckLinkDisplayMode> sdk_mode;
        while (mode_iterator->Next(sdk_mode.releaseAndGetAddressOf()) == S_OK) {
            mode_info_s mode{};
            mode.mode = sdk_mode->GetDisplayMode();
            mode.dim  = {sdk_mode->GetWidth(), sdk_mode->GetHeight()};
            if (sdk_mode->GetFrameRate(&mode.frame_duration, &mode.time_scale) != S_OK) {
                continue;
            }

            mode.colorspace       = detail::get_display_mode_colorspace(sdk_mode.get());
            const auto transfer   = detail::get_color_transfer(mode.colorspace);
            mode.yuv_conversion   = gpu::get_color_transfer_to_yuv(transfer);
            mode.gamut_conversion = gpu::get_gamut_transfer_from_rec709(transfer);

            auto name = decklink_registry_s::get_display_mode_name(sdk_mode.get());
            display_modes.emplace(std::move(name), mode);
        }

        std::vector<std::string> mode_names;
        mode_names.reserve(display_modes.size());
        for (const auto& [name, _] : display_modes) {
            mode_names.push_back(name);
        }
        sr->write(id_, "display_modes", nlohmann::json(mode_names));

        device_        = std::move(device);
        display_modes_ = std::move(display_modes);
        in_use.emplace(device_.get());
        display_mode_name_.commit(display_mode);
        if (!start_playback(app, display_mode)) {
            log()->error("Failed to start DeckLink output {} with {}", device_name, display_mode);
            return;
        }
        sr->write(id_, "connected", true);
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto texture = iface_tex_.resolve_value(app, nodes, state);
        if (texture == nullptr) {
            return;
        }

        if (!callback_ || !device_) {
            return;
        }

        auto target = download_stream_ ? download_stream_->try_acquire() : std::nullopt;
        if (!target) {
            return;
        }

        if (!draw_state_scale_) {
            draw_state_scale_ = std::make_unique<gpu::draw_state_s>();
            auto shader       = app->ctx()->get_shader(gpu::shader_program_s::name_e::basic);
            draw_state_scale_->set_shader_program(shader);
            draw_state_scale_->set_vertex_data(gpu::full_screen_quad_verts_flip_uv);
        }

        if (!draw_state_yuv_) {
            draw_state_yuv_ = std::make_unique<gpu::draw_state_s>();
            auto shader     = app->ctx()->get_shader(gpu::shader_program_s::name_e::rgb_to_yuv);
            draw_state_yuv_->set_shader_program(shader);
            draw_state_yuv_->set_vertex_data(gpu::full_screen_quad_verts_flip_uv);
        }

        const auto dim = display_mode_->dim;

        const gpu::vec2i_t draw_dim{dim.x / 6 * 4, dim.y};

        if (!framebuffer_scale_ || framebuffer_scale_->texture()->texture_dimensions() != dim) {
            framebuffer_scale_ = std::make_unique<gpu::framebuffer_s>(dim, gpu::texture_s::format_e::rgb_f16);
        }

        {
            auto shader = draw_state_scale_->get_shader_program();
            shader->set_uniform("offset", gpu::vec2_t(0, 0));
            shader->set_uniform("scale", gpu::vec2_t(1, 1));
            shader->set_uniform("opacity", 1.0);

            framebuffer_scale_->begin_render(gpu::framebuffer_s::load_op_e::clear);

            texture->bind(0);
            draw_state_scale_->draw();
            gpu::texture_s::unbind(0);
            gpu::framebuffer_s::end_render();
        }

        target->framebuffer()->begin_render(
            {
                .pos  = {0, 0},
                .size = draw_dim,
        },
            gpu::framebuffer_s::load_op_e::clear);

        auto shader = draw_state_yuv_->get_shader_program();
        shader->set_uniform("offset", {0, 0});
        shader->set_uniform("scale", {1.0, 1.0});
        shader->set_uniform("target_width", draw_dim.x);

        shader->set_uniform("transfer", display_mode_->yuv_conversion.matrix);
        shader->set_uniform("transfer_offset", display_mode_->yuv_conversion.offset);
        shader->set_uniform("gamut_transfer", display_mode_->gamut_conversion);

        framebuffer_scale_->texture()->bind(0);
        draw_state_yuv_->draw();
        gpu::texture_s::unbind(0);

        gpu::framebuffer_s::end_render();

        target->submit();
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
