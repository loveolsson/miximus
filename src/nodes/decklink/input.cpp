#include "core/app_state.hpp"
#include "detail/allocator.hpp"
#include "gpu/color_transfer.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/glad.hpp"
#include "gpu/texture.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/validate_option.hpp"
#include "registry.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <memory>
#include <optional>
#include <queue>
#include <unordered_set>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace miximus::nodes::decklink;

using texture_ptr_t = std::unique_ptr<gpu::texture_s>;
using frame_info_t  = std::pair<decklink_ptr<IDeckLinkVideoFrame>, gpu::transfer::transfer_i*>;

class node_impl;

auto log() { return getlog("decklink"); }

class callback_s : public IDeckLinkInputCallback
{
    std::atomic_ulong                 ref_count_{1};
    std::shared_ptr<gpu::context_s>   ctx_;
    std::mutex                        frame_mutex_;
    std::queue<frame_info_t>          frame_queue_;
    decklink_ptr<detail::allocator_s> allocator_;
    BMDDisplayMode                    new_display_mode_{bmdModeUnknown};
    BMDDisplayModeFlags               new_display_mode_flags{};

  public:
    callback_s(std::shared_ptr<gpu::context_s> ctx, decklink_ptr<detail::allocator_s> allocator)
        : ctx_(std::move(ctx))
        , allocator_(std::move(allocator))
    {
    }

    /**
     * IUnknown
     */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID /*iid*/, LPVOID* /*ppv*/) final { return E_NOTIMPL; }

    ULONG STDMETHODCALLTYPE AddRef() final { return ++ref_count_; }

    ULONG STDMETHODCALLTYPE Release() final
    {
        ULONG count = --ref_count_;
        if (count == 0) {
            delete this;
        }
        return count;
    }

    /**
     * IDeckLinkInputCallback
     */
    HRESULT
    VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* /*audioPacket*/) final
    {
        if (videoFrame == nullptr) {
            return S_OK;
        }

        auto lock = ctx_->get_lock();

        void* ptr{};
        videoFrame->GetBytes(&ptr);

        auto* transfer = allocator_->get_transfer(ptr);
        if (transfer == nullptr) {
            return S_OK;
        }

        ctx_->make_current();

        {
            std::unique_lock lock(frame_mutex_);
            if (frame_queue_.size() < 2) {
                transfer->perform_copy();
                frame_queue_.emplace(decklink_ptr<IDeckLinkVideoFrame>::make_owner(videoFrame), transfer);
            }
        }

        gpu::context_s::rewind_current();
        return S_OK;
    }

    HRESULT
    VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                            IDeckLinkDisplayMode*            newDisplayMode,
                            BMDDetectedVideoInputFormatFlags /*detectedSignalFlags*/) final
    {
        if (notificationEvents & bmdVideoInputDisplayModeChanged) {
            std::unique_lock lock(frame_mutex_);
            new_display_mode_      = newDisplayMode->GetDisplayMode();
            new_display_mode_flags = newDisplayMode->GetFlags();
        }

        return S_OK;
    }

    std::optional<frame_info_t> get_frame_from_queue()
    {
        std::unique_lock lock(frame_mutex_);

        if (frame_queue_.empty()) {
            return std::nullopt;
        }

        auto res = std::move(frame_queue_.front());
        frame_queue_.pop();

        return res;
    }

    std::pair<BMDDisplayMode, BMDDisplayModeFlags> get_new_display_mode()
    {
        std::unique_lock lock(frame_mutex_);

        auto res = new_display_mode_;

        new_display_mode_ = bmdModeUnknown;

        return {res, new_display_mode_flags};
    }
};

class node_impl : public node_i
{
    static inline std::unordered_set<IDeckLinkInput*> devices_in_use;
    decklink_ptr<IDeckLinkInput>                      device_;
    decklink_ptr<callback_s>                          callback_;
    decklink_ptr<detail::allocator_s>                 allocator_;

    std::unique_ptr<gpu::texture_s>     texture_;
    std::unique_ptr<gpu::framebuffer_s> framebuffer_;
    std::unique_ptr<gpu::draw_state_s>  draw_state_;
    gpu::color_transfer_e               color_transfer_ = gpu::color_transfer_e::Rec601;

    std::optional<frame_info_t> processed_frame_;

    output_interface_s<gpu::texture_s*> iface_tex_;

    void free_device()
    {
        devices_in_use.erase(device_.ptr());

        device_->StopStreams();
        device_->DisableAudioInput();
        device_->DisableVideoInput();
        device_->FlushStreams();

        processed_frame_ = std::nullopt;

        if (texture_ && allocator_) {
            allocator_->unregister_texture(texture_.get());
        }

        device_    = nullptr;
        allocator_ = nullptr;
        callback_  = nullptr;
        texture_.reset();
        framebuffer_.reset();
        color_transfer_ = gpu::color_transfer_e::Rec601;
    }

  public:
    explicit node_impl() { interfaces_.emplace("tex", &iface_tex_); }

    ~node_impl() override
    {
        if (device_) {
            free_device();
        }
    }

    node_impl(const node_impl&) = delete;
    node_impl(node_impl&&)      = delete;
    void operator=(const node_impl&) = delete;
    void operator=(node_impl&&) = delete;

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* traits) final
    {
        traits->must_run = true;

        if (callback_) {
            processed_frame_ = callback_->get_frame_from_queue();

            auto [new_mode, flags] = callback_->get_new_display_mode();
            if (new_mode != bmdModeUnknown && device_) {
                device_->PauseStreams();
                device_->EnableVideoInput(new_mode, bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
                device_->StartStreams();

                if (flags & bmdDisplayModeColorspaceRec709) {
                    color_transfer_ = gpu::color_transfer_e::Rec709;
                } else {
                    color_transfer_ = gpu::color_transfer_e::Rec601;
                }
            }
        }

        auto device_name = state.get_option<std::string>("device_name");
        auto enabled     = state.get_option<bool>("enabled");

        auto device = enabled ? app->decklink_registry()->get_input(device_name) : nullptr;
        if (device == device_) {
            return;
        }

        if (device_) {
            free_device();
        }

        if (!device) {
            return;
        }

        if (devices_in_use.count(device.ptr()) > 0) {
            return;
        }

        log()->info("Setting up DeckLink input {}", device_name);
        device_ = device;
        devices_in_use.emplace(device_.ptr());

        auto ctx = gpu::context_s::create_shared_context(false, app->ctx());

        allocator_ = new detail::allocator_s(ctx, gpu::transfer::transfer_i::direction_e::cpu_to_gpu);
        device_->SetVideoInputFrameMemoryAllocator(allocator_.ptr());

        callback_ = new callback_s(ctx, allocator_);
        device_->SetCallback(callback_.ptr());

        device_->EnableVideoInput(bmdModeNTSC, bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
        device_->StartStreams();
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        if (!allocator_) {
            return;
        }

        if (processed_frame_) {
            auto*        frame = processed_frame_->first.ptr();
            gpu::vec2i_t frame_dims{frame->GetWidth(), frame->GetHeight()};

            if (!texture_ || texture_->display_dimensions() != frame_dims) {
                if (texture_) {
                    allocator_->unregister_texture(texture_.get());
                }

                using color_e = gpu::texture_s::colorspace_e;

                texture_     = std::make_unique<gpu::texture_s>(frame_dims, color_e::UYVY);
                framebuffer_ = std::make_unique<gpu::framebuffer_s>(frame_dims, color_e::RGB);
                allocator_->register_texture(texture_.get());
            }

            auto* transfer = processed_frame_->second;
            transfer->wait_for_copy();
            transfer->perform_transfer(texture_.get());
            allocator_->begin_texture_use(texture_.get());

            framebuffer_->bind();
            glViewport(0, 0, frame_dims.x, frame_dims.y);

            if (!draw_state_) {
                using shader_name = gpu::shader_program_s::name_e;
                draw_state_       = std::make_unique<gpu::draw_state_s>();
                auto* shader      = app->ctx()->get_shader(shader_name::yuv_to_rgb);
                draw_state_->set_shader_program(shader);
                draw_state_->set_vertex_data(gpu::full_screen_quad_verts);
            }

            auto* shader = draw_state_->get_shader_program();
            shader->set_uniform("offset", gpu::vec2_t{0, 1.0});
            shader->set_uniform("scale", gpu::vec2_t{1.0, -1.0});
            shader->set_uniform("opacity", 1.0);

            auto color_transfer = gpu::get_color_transfer(color_transfer_);
            shader->set_uniform("transfer", color_transfer);

            texture_->bind(0);
            draw_state_->draw();
            gpu::texture_s::unbind(0);
            gpu::framebuffer_s::unbind();

            framebuffer_->texture()->generate_mip_maps();
        }

        if (framebuffer_) {
            iface_tex_.set_value(framebuffer_->texture());
        }
    }

    void complete(core::app_state_s* /*app*/) final
    {
        if (processed_frame_ && allocator_ && texture_) {
            allocator_->end_texture_use(texture_.get());
        }

        processed_frame_ = std::nullopt;
        iface_tex_.set_value(nullptr);
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "DeckLink input"},
            {"enabled", true},
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "device_name") {
            return validate_option<std::string_view>(value);
        }

        if (name == "enabled") {
            return validate_option<bool>(value);
        }

        return false;
    }

    std::string_view type() const final { return "decklink_input"; }
};
} // namespace

namespace miximus::nodes::decklink {

std::shared_ptr<node_i> create_input_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::decklink
