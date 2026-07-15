#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "detail/allocator.hpp"
#include "detail/colorspace.hpp"
#include "gpu/color_transfer.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/sync.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/transfer.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"
#include "registry.hpp"
#include "utils/frame_queue.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <atomic>
#include <limits>
#include <memory>
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

// A frame that has already been uploaded to a UYUV GPU texture by the callback.
struct frame_info_s
{
    std::unique_ptr<gpu::texture_s> texture; // 10-bit UYUV, pre-uploaded
    std::unique_ptr<gpu::sync_s>    sync;
    gpu::vec2i_t                    tx_dim{};  // texture dimensions (row_bytes/4 x height)
    gpu::vec2i_t                    src_dim{}; // display dimensions (width x height)
    BMDColorspace                   colorspace{bmdColorspaceRec709};

    frame_info_s()  = default;
    ~frame_info_s() = default;

    frame_info_s(const frame_info_s&)                = delete;
    frame_info_s& operator=(const frame_info_s&)     = delete;
    frame_info_s(frame_info_s&&) noexcept            = default;
    frame_info_s& operator=(frame_info_s&&) noexcept = default;
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

    std::shared_ptr<gpu::context_s>        ctx_;
    decklink_ptr<IDeckLinkVideoConversion> converter_;
    decklink_ptr<allocator_s>              allocator_;

    utils::frame_queue_s<frame_info_s> frames_rendered_;
    utils::frame_queue_s<frame_info_s> frames_free_;

    std::atomic<BMDDisplayMode>    new_display_mode_{bmdModeUnknown};
    std::atomic<BMDFieldDominance> frame_field_dominance_{bmdUnknownFieldDominance};
    std::atomic<BMDColorspace>     colorspace_{bmdColorspaceRec709};

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

  public:
    explicit callback_s(std::shared_ptr<gpu::context_s> ctx)
        : ctx_(std::move(ctx))
        , converter_(decklink_registry_s::get_converter())
    {
        // Pre-allocate free frame slots (texture will be created lazily on first capture).
        for (int i = 0; i < 5; i++) {
            frames_free_.push_frame({});
        }

        // Allocator for CPU→GPU: DeckLink writes directly into our transfer buffer.
        allocator_ = make_decklink_ptr<allocator_s>(ctx_, gpu::transfer::transfer_i::direction_e::cpu_to_gpu);
    }

    ~callback_s() override
    {
        const gpu::context_scope_s context_scope(*ctx_, gpu::context_lock_e::lock);

        // Unregister textures from transfer backend before destroying.
        for_each_frame([&](frame_info_s& f) {
            if (f.texture) {
                gpu::transfer::transfer_i::unregister_texture(gpu::transfer::transfer_i::get_prefered_type(),
                                                              f.texture.get());
            }
        });

        allocator_->destroy_free_transfers();
        frames_rendered_.clear();
        frames_free_.clear();
    }

    callback_s(const callback_s&)            = delete;
    callback_s& operator=(const callback_s&) = delete;
    callback_s(callback_s&&)                 = delete;
    callback_s& operator=(callback_s&&)      = delete;

    // ── IDeckLinkVideoBufferAllocatorProvider ────────────────────────────────

    HRESULT STDMETHODCALLTYPE GetVideoBufferAllocator(uint32_t bufferSize,
                                                      uint32_t /*width*/,
                                                      uint32_t /*height*/,
                                                      uint32_t /*rowBytes*/,
                                                      BMDPixelFormat                  pixelFormat,
                                                      IDeckLinkVideoBufferAllocator** outAllocator) final
    {
        // Only hand over our allocator for the primary capture format.
        // Conversion frames (different pixel format) use DeckLink's default.
        if (pixelFormat != bmdFormat10BitYUV) {
            *outAllocator = nullptr;
            return E_NOTIMPL;
        }

        allocator_->set_buffer_size(bufferSize);
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

        auto pop = frames_free_.pop_frame();
        if (!pop.first.has_value()) {
            log()->warn("VideoInputFrameArrived dropped frame");
            return S_OK;
        }

        const gpu::context_scope_s context_scope(*ctx_, gpu::context_lock_e::lock);

        auto& frame = pop.first->frame;

        const GLsizeiptr   row_bytes = videoFrame->GetRowBytes();
        const gpu::vec2i_t tx_dim{static_cast<int>(row_bytes) / 4, videoFrame->GetHeight()};
        const gpu::vec2i_t src_dim{videoFrame->GetWidth(), tx_dim.y};
        frame.colorspace = get_frame_colorspace(videoFrame);

        // Ensure texture dimensions match.
        if (!frame.texture || frame.tx_dim != tx_dim) {
            if (frame.texture) {
                gpu::transfer::transfer_i::unregister_texture(gpu::transfer::transfer_i::get_prefered_type(),
                                                              frame.texture.get());
            }
            frame.texture = std::make_unique<gpu::texture_s>(tx_dim, gpu::texture_s::format_e::uyuv_u10);
            gpu::transfer::transfer_i::register_texture(gpu::transfer::transfer_i::get_prefered_type(),
                                                        frame.texture.get());
            frame.tx_dim  = tx_dim;
            frame.src_dim = src_dim;
        }

        // Try to get the transfer from our allocator (DeckLink wrote directly into it).
        IDeckLinkVideoBuffer* video_buffer = nullptr;
        if (videoFrame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&video_buffer)) == S_OK) {
            auto* transfer = allocator_s::get_transfer(video_buffer);
            video_buffer->Release();

            if (transfer != nullptr) {
                // Allocator path: transfer buffer already has the captured data.
                // Signals differ per backend: for persistent this copies ptr→PBO,
                // for CUDA/DVP this is a DMA signal.
                gpu::transfer::transfer_i::end_texture_use(gpu::transfer::transfer_i::get_prefered_type(),
                                                           frame.texture.get());
                transfer->perform_copy();
                transfer->perform_transfer(frame.texture.get());
                transfer->wait_for_copy();

                frame.sync = std::make_unique<gpu::sync_s>();
                frames_rendered_.push_frame(std::move(frame), frame_arrival_time);
                return S_OK;
            }
        }

        // Fallback path: DeckLink used its own buffer (e.g. internal conversion frame).
        // Map the video buffer and copy manually into a GL PBO → texture.
        if (video_buffer == nullptr) {
            videoFrame->QueryInterface(IID_IDeckLinkVideoBuffer, reinterpret_cast<void**>(&video_buffer));
        }

        if (video_buffer != nullptr) {
            if (video_buffer->StartAccess(bmdBufferAccessRead) == S_OK) {
                void* src_data = nullptr;
                video_buffer->GetBytes(&src_data);

                frame.texture->upload(src_data);
                video_buffer->EndAccess(bmdBufferAccessRead);
            }
            video_buffer->Release();
        }

        frame.sync = std::make_unique<gpu::sync_s>();
        frames_rendered_.push_frame(std::move(frame), frame_arrival_time);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                                                      IDeckLinkDisplayMode*            newDisplayMode,
                                                      BMDDetectedVideoInputFormatFlags /*detectedSignalFlags*/) final
    {
        const bool display_mode_changed = (notificationEvents & bmdVideoInputDisplayModeChanged) != 0;
        const bool colorspace_changed   = (notificationEvents & bmdVideoInputColorspaceChanged) != 0;

        if (display_mode_changed) {
            new_display_mode_      = newDisplayMode->GetDisplayMode();
            frame_field_dominance_ = newDisplayMode->GetFieldDominance();
        }
        if (display_mode_changed || colorspace_changed) {
            colorspace_ = get_display_mode_colorspace(newDisplayMode);
        }
        return S_OK;
    }

    void push_free_frame(frame_info_s&& frame) { frames_free_.push_frame(std::move(frame)); }

    std::optional<frame_info_s> get_rendered_frame(utils::flicks pts, utils::flicks flush)
    {
        std::optional<frame_info_s> res;
        utils::flicks               timestamp{};

        decltype(frames_rendered_)::record_s record;
        while ((!res || timestamp < flush) && frames_rendered_.pop_frame_if_older(pts, &record)) {
            if (res) {
                res->sync.reset();
                frames_free_.push_frame(std::move(*res));
            }
            res       = std::move(record.frame);
            timestamp = record.pts;
        }
        return res;
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

  private:
    template <typename Fn>
    void for_each_frame(Fn fn)
    {
        // Apply fn to all frames in both queues (used for cleanup).
        // We access them by draining into a temp vector and restoring.
        std::vector<frame_info_s>        tmp;
        decltype(frames_free_)::record_s r;
        while (frames_free_.pop_frame(&r)) {
            tmp.push_back(std::move(r.frame));
        }
        decltype(frames_rendered_)::record_s rr;
        while (frames_rendered_.pop_frame(&rr)) {
            tmp.push_back(std::move(rr.frame));
        }
        for (auto& f : tmp) {
            fn(f);
        }
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

            auto ctx  = gpu::context_s::create_shared_context(false, app->ctx());
            callback_ = make_decklink_ptr<callback_s>(ctx);
            device_->SetCallback(callback_.get());

            // Use our allocator provider so DeckLink DMA's into our transfer buffers.
            device_->EnableVideoInputWithAllocatorProvider(
                bmdModeNTSC, bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection, callback_.get());
            device_->StartStreams();
        }
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        if (!work_frame_ || !work_frame_->texture) {
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

        // The UYUV texture was already uploaded in the callback — just wait for the sync.
        if (work_frame_->sync) {
            work_frame_->sync->gpu_wait();
        }

        // Signal the transfer layer that GL is now using the texture.
        gpu::transfer::transfer_i::begin_texture_use(gpu::transfer::transfer_i::get_prefered_type(),
                                                     work_frame_->texture.get());

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

    void complete(core::app_state_s* /*app*/) final
    {
        if (work_frame_ && callback_) {
            work_frame_->sync.reset();
            callback_->push_free_frame(std::move(*work_frame_));
        }
        work_frame_.reset();
    }

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
