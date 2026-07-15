#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/allocator.hpp"
#include "detail/colorspace.hpp"
#include "gpu/color_transfer.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <atomic>
#include <cstring>
#include <limits>
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
using namespace std::chrono_literals;

auto log() { return getlog("decklink"); }

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
    decklink_ptr<allocator_s>                               allocator_;
    std::mutex                                              upload_mutex_;
    std::shared_ptr<gpu::transfer::texture_upload_stream_s> upload_stream_;
    std::map<uint64_t, upload_metadata_s>                   upload_metadata_;

    std::atomic<BMDDisplayMode> new_display_mode_{bmdModeUnknown};
    std::atomic<BMDColorspace>  colorspace_{bmdColorspaceRec709};

    BMDColorspace get_frame_colorspace(IDeckLinkVideoInputFrame* frame) const
    {
        IDeckLinkVideoFrameMetadataExtensions* metadata = nullptr;
        if (frame->QueryInterface(IID_IDeckLinkVideoFrameMetadataExtensions, reinterpret_cast<void**>(&metadata)) ==
            S_OK) {
            int64_t value = 0;
            if (metadata->GetInt(bmdDeckLinkFrameMetadataColorspace, &value) == S_OK) {
                metadata->Release();
                const auto colorspace = static_cast<BMDColorspace>(value);
                if (colorspace == bmdColorspaceRec601 || colorspace == bmdColorspaceRec709 ||
                    colorspace == bmdColorspaceRec2020) {
                    return colorspace;
                }
                return colorspace_.load();
            }
            metadata->Release();
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
    explicit callback_s(gpu::transfer::texture_upload_service_s* upload_service)
        : upload_service_(upload_service)
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
        // Only hand over our allocator for the primary capture format.
        // Conversion frames (different pixel format) use DeckLink's default.
        if (pixelFormat != bmdFormat10BitYUV) {
            *outAllocator = nullptr;
            return E_NOTIMPL;
        }

        auto stream = upload_service_->create_stream({
            .dimensions        = {static_cast<int>(rowBytes / 4), static_cast<int>(height)},
            .format            = gpu::texture_s::format_e::uyuv_u10,
            .byte_size         = bufferSize,
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
    }

    // ── IDeckLinkInputCallback ───────────────────────────────────────────────

    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame,
                                                     IDeckLinkAudioInputPacket* /*audioPacket*/) final
    {
        const auto frame_arrival_time = utils::flicks_now();

        if (videoFrame == nullptr) {
            return S_OK;
        }

        const auto         row_bytes = static_cast<size_t>(videoFrame->GetRowBytes());
        const gpu::vec2i_t src_dim{videoFrame->GetWidth(), videoFrame->GetHeight()};
        const auto         frame_size = row_bytes * static_cast<size_t>(src_dim.y);

        std::optional<gpu::transfer::texture_upload_lease_s> fallback_upload;
        uint64_t                                             version{};
        decklink_ptr<IDeckLinkVideoBuffer>                   video_buffer;
        if (videoFrame->QueryInterface(IID_IDeckLinkVideoBuffer,
                                       reinterpret_cast<void**>(video_buffer.releaseAndGetAddressOf())) == S_OK) {
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
                const std::scoped_lock lock(upload_mutex_);
                upload_metadata_.erase(version);
            }
        } else if (fallback_upload) {
            fallback_upload->submit();
        }
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                                                      IDeckLinkDisplayMode*            newDisplayMode,
                                                      BMDDetectedVideoInputFormatFlags /*detectedSignalFlags*/) final
    {
        const bool display_mode_changed = (notificationEvents & bmdVideoInputDisplayModeChanged) != 0;
        const bool colorspace_changed   = (notificationEvents & bmdVideoInputColorspaceChanged) != 0;

        if (display_mode_changed) {
            new_display_mode_ = newDisplayMode->GetDisplayMode();
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

    // ── IUnknown ─────────────────────────────────────────────────────────────

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) final
    {
        if (memcmp(&iid, &IID_IDeckLinkVideoBufferAllocatorProvider, sizeof(REFIID)) == 0) {
            *ppv = static_cast<IDeckLinkVideoBufferAllocatorProvider*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOTIMPL;
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

    std::unique_ptr<gpu::framebuffer_s> framebuffer_;
    std::unique_ptr<gpu::draw_state_s>  draw_state_;
    std::optional<frame_info_s>         work_frame_;
    uint64_t                            last_device_version_{std::numeric_limits<uint64_t>::max()};
    BMDColorspace                       colorspace_{bmdColorspaceRec709};
    gpu::color_conversion_s yuv_conversion_{gpu::get_color_transfer_from_yuv(gpu::color_transfer_e::Rec709)};
    gpu::mat3               gamut_conversion_{1.0F};

    output_interface_s<gpu::texture_s*> iface_tex_{*this, "tex"};

    static auto& devices_in_use()
    {
        static std::unordered_set<IDeckLinkInput*> devices;
        return devices;
    }

    void free_device()
    {
        devices_in_use().erase(device_.get());

        device_->StopStreams();
        device_->DisableAudioInput();
        device_->DisableVideoInput();
        device_->FlushStreams();

        device_   = nullptr;
        callback_ = nullptr;
        framebuffer_.reset();
        work_frame_.reset();
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
        if (current_version != last_device_version_) {
            last_device_version_ = current_version;
            sr->write(id_, "device_names", nlohmann::json(app->decklink_registry()->get_input_names()));
        }

        sr->write(id_, "connected", device_ != nullptr);

        if (device_ && callback_) {
            auto pts    = app->frame_info.timestamp;
            auto flush  = pts - app->frame_info.duration * 2;
            work_frame_ = callback_->get_rendered_frame(pts, flush);

            auto new_mode = callback_->get_new_display_mode();
            if (new_mode != bmdModeUnknown) {
                device_->PauseStreams();
                device_->EnableVideoInputWithAllocatorProvider(
                    new_mode, bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection, callback_.get());
                device_->StartStreams();
            }
        }

        auto device_name = state.get_option<std::string>("device_name");
        auto enabled     = state.get_option<bool>("enabled");

        auto device = enabled ? app->decklink_registry()->get_input(device_name) : nullptr;
        if (device != device_) {
            if (device_) {
                free_device();
            }

            if (!device || devices_in_use().contains(device.get())) {
                return;
            }

            log()->info("Setting up DeckLink input {}", device_name);
            device_ = device;
            devices_in_use().emplace(device_.get());

            callback_ = make_decklink_ptr<callback_s>(app->texture_upload_service());
            device_->SetCallback(callback_.get());

            // Use our allocator provider so DeckLink DMA's into our transfer buffers.
            device_->EnableVideoInputWithAllocatorProvider(
                bmdModeNTSC, bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection, callback_.get());
            device_->StartStreams();
        }
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        if (!work_frame_ || work_frame_->texture == nullptr) {
            iface_tex_.set_value(framebuffer_ ? framebuffer_->texture() : nullptr);
            return;
        }

        if (!draw_state_) {
            draw_state_ = std::make_unique<gpu::draw_state_s>();
            auto shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::yuv_to_rgb);
            draw_state_->set_shader_program(shader);
            draw_state_->set_vertex_data(gpu::full_screen_quad_verts_flip_uv);
        }

        const auto src_dim = work_frame_->src_dim;

        if (!framebuffer_ || framebuffer_->texture()->texture_dimensions() != src_dim) {
            framebuffer_ = std::make_unique<gpu::framebuffer_s>(src_dim, gpu::texture_s::format_e::rgb_f16);
        }

        auto shader = draw_state_->get_shader_program();
        shader->set_uniform("offset", gpu::vec2i_t{0, 0});
        shader->set_uniform("scale", gpu::vec2i_t{1.0, 1.0});
        shader->set_uniform("target_width", src_dim.x);

        if (colorspace_ != work_frame_->colorspace) {
            colorspace_         = work_frame_->colorspace;
            const auto transfer = get_color_transfer(colorspace_);
            yuv_conversion_     = gpu::get_color_transfer_from_yuv(transfer);
            gamut_conversion_   = gpu::get_gamut_transfer_to_rec709(transfer);
        }

        shader->set_uniform("transfer", yuv_conversion_.matrix);
        shader->set_uniform("transfer_offset", yuv_conversion_.offset);
        shader->set_uniform("gamut_transfer", gamut_conversion_);

        framebuffer_->begin_render(gpu::framebuffer_s::load_op_e::clear);
        work_frame_->texture->bind(0);
        draw_state_->draw();
        gpu::texture_s::unbind(0);
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
