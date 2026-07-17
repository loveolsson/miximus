#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/colorspace.hpp"
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
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_set>
#include <utility>

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

    std::shared_ptr<gpu::transfer::texture_download_stream_s> download_stream_;
    decklink_ptr<IDeckLinkVideoFrame>                         last_frame_;

    IDeckLinkOutput* device_;
    mode_info_s      mode_info_;
    BMDTimeValue     pts_{0};

    void set_colorspace_metadata(IDeckLinkMutableVideoFrame* frame) const
    {
        IDeckLinkVideoFrameMutableMetadataExtensions* metadata = nullptr;
        if (frame->QueryInterface(IID_IDeckLinkVideoFrameMutableMetadataExtensions,
                                  reinterpret_cast<void**>(&metadata)) == S_OK) {
            (void)metadata->SetInt(bmdDeckLinkFrameMetadataColorspace, mode_info_.colorspace);
            metadata->Release();
        }
    }

  public:
    callback_s(std::shared_ptr<gpu::transfer::texture_download_stream_s> download_stream,
               IDeckLinkOutput*                                          device,
               mode_info_s                                               mode_info)
        : download_stream_(std::move(download_stream))
        , device_(device)
        , mode_info_(mode_info)
    {
        IDeckLinkMutableVideoFrame* frame = nullptr;

        if (SUCCEEDED(device_->CreateVideoFrame(
                mode_info_.dim.x, mode_info_.dim.y, mode_info.dim.x * 2, bmdFormat8BitYUV, 0, &frame))) {
            set_colorspace_metadata(frame);
            IDeckLinkVideoBuffer* buffer = nullptr;
            if (frame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&buffer)) == S_OK) {
                buffer->StartAccess(bmdBufferAccessWrite);
                uint16_t* data = nullptr;
                buffer->GetBytes(reinterpret_cast<void**>(&data));
                std::fill(data, data + static_cast<size_t>(mode_info_.dim.x * mode_info_.dim.y), 0x0000);
                buffer->EndAccess(bmdBufferAccessWrite);
                buffer->Release();
            }

            for (int i = 0; i < 4; i++) {
                device_->ScheduleVideoFrame(frame, pts_, mode_info_.frame_duration, mode_info_.time_scale);
            }

            last_frame_ = frame;
            frame->Release();
        }
    }

    ~callback_s() override = default;

    callback_s(callback_s&&)                 = delete;
    callback_s(const callback_s&)            = delete;
    callback_s& operator=(callback_s&&)      = delete;
    callback_s& operator=(const callback_s&) = delete;

    /**
     * IUnknown
     */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID /*iid*/, LPVOID* /*ppv*/) final { return E_NOTIMPL; }

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
        if (result == bmdOutputFrameFlushed) {
            return S_OK;
        }

        auto frame = download_stream_->try_consume_latest();
        if (frame) {
            IDeckLinkMutableVideoFrame* dst_frame = nullptr;
            const int32_t               row_bytes = ((mode_info_.dim.x + 47) / 48) * 128;

            if (SUCCEEDED(device_->CreateVideoFrame(
                    mode_info_.dim.x, mode_info_.dim.y, row_bytes, bmdFormat10BitYUV, 0, &dst_frame))) {
                set_colorspace_metadata(dst_frame);
                IDeckLinkVideoBuffer* dst_buffer = nullptr;
                if (dst_frame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&dst_buffer)) ==
                    S_OK) {
                    dst_buffer->StartAccess(bmdBufferAccessWrite);
                    void* dst_ptr = nullptr;
                    dst_buffer->GetBytes(&dst_ptr);
                    const auto size = static_cast<size_t>(row_bytes) * static_cast<size_t>(mode_info_.dim.y);
                    assert(size == frame->size());
                    memcpy(dst_ptr, frame->ptr(), size);
                    dst_buffer->EndAccess(bmdBufferAccessWrite);
                    dst_buffer->Release();
                }

                last_frame_ = dst_frame;
                dst_frame->Release();
            }
        }

        if (last_frame_) {
            auto duration = mode_info_.frame_duration;

            if (result != bmdOutputFrameCompleted) {
                duration *= 2;
                log()->warn("Frame dropped");
            }

            device_->ScheduleVideoFrame(last_frame_.get(), pts_, duration, mode_info_.time_scale);
            pts_ += duration;
        }

        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() final
    {
        log()->warn("Playback has stopped");
        return S_OK;
    }

    /**
     * IDeckLinkAudioOutputCallback
     */
    HRESULT STDMETHODCALLTYPE RenderAudioSamples(BOOL /*preroll*/) final { return S_OK; }
};
#pragma GCC diagnostic pop

class node_impl : public node_i
{
    decklink_ptr<IDeckLinkOutput>                   device_;
    decklink_ptr<callback_s>                        callback_;
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

    void free_device()
    {
        devices_in_use().erase(device_.get());
        device_->StopScheduledPlayback(0, nullptr, 1.0);

        device_   = nullptr;
        callback_ = nullptr;
        framebuffer_scale_.reset();
        download_stream_.reset();
        display_modes_.clear();
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

    void stop_playback()
    {
        callback_ = nullptr;

        if (!device_) {
            return;
        }

        auto res = device_->StopScheduledPlayback(0, nullptr, 0);
        (void)res;
        // assert(res == S_OK);

        res = device_->DisableVideoOutput();
        (void)res;
        // assert(res == S_OK);

        res = device_->DisableAudioOutput();
        (void)res;
        // assert(res == S_OK);

        res = device_->FlushBufferedAudioSamples();
        (void)res;
        // assert(res == S_OK);
    }

    void restart_playback(core::app_state_s* app)
    {
        // assert(device_);
        stop_playback();

        auto mode_it = display_modes_.find(display_mode_name_.value());
        if (mode_it == display_modes_.end()) {
            return;
        }

        display_mode_ = &mode_it->second;

        log()->info("Enabling video output with {}", display_mode_name_.value());

        auto res = device_->EnableVideoOutput(display_mode_->mode, bmdVideoOutputFlagDefault);
        assert(res == S_OK);
        (void)res;

        const int    row_pixels = ((display_mode_->dim.x + 47) / 48) * 32;
        const size_t frame_size = static_cast<size_t>(row_pixels) * static_cast<size_t>(display_mode_->dim.y) * 4;
        download_stream_        = app->texture_download_service()->create_stream({
                   .dimensions = {row_pixels, display_mode_->dim.y},
                   .format     = gpu::texture_s::format_e::uyuv_u10,
                   .byte_size  = frame_size,
                   .max_slots  = 7,
        });
        callback_               = make_decklink_ptr<callback_s>(download_stream_, device_.get(), *display_mode_);
        res                     = device_->SetScheduledFrameCompletionCallback(callback_.get());
        assert(res == S_OK);
        (void)res;

        res = device_->SetAudioCallback(callback_.get());
        assert(res == S_OK);
        (void)res;

        res = device_->StartScheduledPlayback(0, display_mode_->time_scale, 1.0);
        assert(res == S_OK);
        (void)res;
    }

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* traits) final
    {
        traits->must_run = true;
        auto* sr         = app->status_registry();

        // Rebuild device list only when the registry has changed
        const auto current_version = app->decklink_registry()->get_device_list_version();
        if (device_version_.observe(current_version)) {
            sr->write(id_, "device_names", nlohmann::json(app->decklink_registry()->get_output_names()));
        }

        sr->write(id_, "connected", device_ != nullptr);

        auto device_name  = state.get_option<std::string>("device_name");
        auto display_mode = state.get_option<std::string>("display_mode");
        auto enabled      = state.get_option<bool>("enabled");

        auto device = enabled ? app->decklink_registry()->get_output(device_name) : nullptr;
        if (device == device_) {
            // Check if the display_mode setting has changed since last frame
            if (display_mode_name_.observe(display_mode)) {
                restart_playback(app);
            }
        } else {
            if (device_) {
                free_device();
                sr->write(id_, "display_modes", nlohmann::json::array());
            }

            auto& in_use = devices_in_use();

            if (!device || in_use.contains(device.get())) {
                return;
            }

            log()->info("Setting up DeckLink output {}", device_name);
            device_ = device;
            in_use.emplace(device_.get());

            IDeckLinkDisplayModeIterator* itr   = nullptr;
            IDeckLinkDisplayMode*         imode = nullptr;

            {
                device_->GetDisplayModeIterator(&itr);

                while (itr->Next(&imode) == S_OK) {
                    mode_info_s mode{};

                    mode.mode = imode->GetDisplayMode();
                    mode.dim  = {imode->GetWidth(), imode->GetHeight()};
                    imode->GetFrameRate(&mode.frame_duration, &mode.time_scale);

                    mode.colorspace       = detail::get_display_mode_colorspace(imode);
                    const auto transfer   = detail::get_color_transfer(mode.colorspace);
                    mode.yuv_conversion   = gpu::get_color_transfer_to_yuv(transfer);
                    mode.gamut_conversion = gpu::get_gamut_transfer_from_rec709(transfer);

                    auto name = decklink_registry_s::get_display_mode_name(imode);
                    display_modes_.emplace(name, mode);

                    imode->Release();
                }

                itr->Release();
            }

            {
                std::vector<std::string> mode_names;
                mode_names.reserve(display_modes_.size());
                for (const auto& [name, _] : display_modes_) {
                    mode_names.push_back(name);
                }
                sr->write(id_, "display_modes", nlohmann::json(mode_names));
            }

            display_mode_name_.commit(display_mode);
            restart_playback(app);
        }
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
            {"name",         "DeckLink input"},
            {"enabled",      true            },
            {"display_mode", "720p60"        },
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
