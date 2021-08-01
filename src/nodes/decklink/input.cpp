#include "input.hpp"
#include "detail/allocator.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/glad.hpp"
#include "gpu/texture.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "registry.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <memory>
#include <optional>
#include <queue>

namespace miximus::nodes::decklink {

using texture_ptr_t = std::unique_ptr<gpu::texture_s>;

class node_impl;

class callback_s : public IDeckLinkInputCallback
{
    std::weak_ptr<node_impl> node_;
    std::atomic_ulong        ref_count_{1};

  public:
    callback_s(std::weak_ptr<node_impl> node)
        : node_(std::move(node))
    {
    }

    /**
     * IUnknown
     */
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) final { return E_NOTIMPL; }

    ULONG STDMETHODCALLTYPE AddRef(void) final { return ++ref_count_; }

    ULONG STDMETHODCALLTYPE Release(void) final
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
    VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioPacket) final;

    HRESULT
    VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                            IDeckLinkDisplayMode*            newDisplayMode,
                            BMDDetectedVideoInputFormatFlags detectedSignalFlags) final;
};

class node_impl
    : public node_i
    , public std::enable_shared_from_this<node_impl>
{
    using if_dir       = interface_i::dir_e;
    using frame_info_t = std::pair<decklink_ptr<IDeckLinkVideoFrame>, gpu::transfer::transfer_i*>;

    static inline std::set<IDeckLinkInput*> devices_in_use;
    decklink_ptr<IDeckLinkInput>            device_;
    decklink_ptr<callback_s>                callback_;
    decklink_ptr<detail::allocator_s>       allocator_;

    std::shared_ptr<gpu::context_s>     allocator_ctx_;
    std::unique_ptr<gpu::texture_s>     texture_;
    std::unique_ptr<gpu::framebuffer_s> framebuffer_;
    std::unique_ptr<gpu::draw_state_s>  draw_state_;

    std::mutex                  frame_mutex_;
    std::queue<frame_info_t>    frame_queue_;
    std::optional<frame_info_t> processed_frame_;

    output_interface_s<gpu::texture_s*> iface_tex_;

    void free_device()
    {
        device_->SetCallback(nullptr);
        device_->StopStreams();
        device_->DisableAudioInput();
        device_->DisableVideoInput();
        device_->FlushStreams();
        device_->SetVideoInputFrameMemoryAllocator(nullptr);

        {
            std::unique_lock lock(frame_mutex_);
            while (!frame_queue_.empty()) {
                frame_queue_.pop();
            }
            processed_frame_ = std::nullopt;
        }

        device_    = nullptr;
        allocator_ = nullptr;
        allocator_ctx_.reset();
        devices_in_use.erase(device_.ptr());
    }

    friend class callback_s;

  public:
    explicit node_impl() { interfaces_.emplace("tex", &iface_tex_); }

    ~node_impl()
    {
        if (device_) {
            free_device();
        }
    }

    void prepare(core::app_state_s& app, const node_state_s& state, traits_s* traits) final
    {
        traits->must_run = true;

        if (device_) {
            std::unique_lock lock(frame_mutex_);
            if (!frame_queue_.empty()) {
                processed_frame_ = std::move(frame_queue_.front());
                frame_queue_.pop();
            }
        }

        auto device_name = state.get_option<std::string>("device_name", "");

        auto device = app.decklink_registry().get_input(device_name);
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

        getlog("app")->info("Setting up DeckLink input {}", device_name);
        device_ = device;
        devices_in_use.emplace(device_.ptr());

        allocator_ctx_ = std::make_shared<gpu::context_s>(false, &app.ctx());
        allocator_     = new detail::allocator_s(allocator_ctx_, gpu::transfer::transfer_i::direction_e::cpu_to_gpu);
        device_->SetVideoInputFrameMemoryAllocator(allocator_.ptr());

        callback_ = new callback_s(this->shared_from_this());
        device_->SetCallback(callback_.ptr());

        device_->EnableVideoInput(bmdModeNTSC, bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
        device_->StartStreams();
    }

    void execute(core::app_state_s& app, const node_map_t& nodes, const node_state_s& state) final
    {
        if (!allocator_) {
            return;
        }

        if (processed_frame_ != std::nullopt) {
            auto*        frame = processed_frame_->first.ptr();
            gpu::vec2i_t frame_dims{frame->GetWidth(), frame->GetHeight()};

            if (!texture_ || texture_->display_dimensions() != frame_dims) {
                if (texture_) {
                    allocator_->unregister_texture(*texture_);
                }

                using color_e = gpu::texture_s::color_type_e;

                texture_     = std::make_unique<gpu::texture_s>(frame_dims, color_e::UYVY);
                framebuffer_ = std::make_unique<gpu::framebuffer_s>(frame_dims, color_e::RGB);
                allocator_->register_texture(*texture_);
            }

            auto* transfer = processed_frame_->second;
            transfer->wait_for_transfer();
            transfer->perform_transfer(*texture_);
            allocator_->begin_texture_use(*texture_);
            framebuffer_->bind();

            glViewport(0, 0, frame_dims.x, frame_dims.y);

            if (!draw_state_) {
                draw_state_  = std::make_unique<gpu::draw_state_s>();
                auto& shader = app.ctx().get_shader(gpu::shader_program_s::name_e::yuv_to_rgb);
                draw_state_->set_shader_program(&shader);
                draw_state_->set_vertex_data(gpu::full_screen_quad_verts);
            }

            auto* shader = draw_state_->get_shader_program();
            shader->set_uniform("offset", gpu::vec2_t{0, 1.0});
            shader->set_uniform("scale", gpu::vec2_t{1.0, -1.0});

            texture_->bind(0);
            draw_state_->draw();
            texture_->unbind(0);

            gpu::framebuffer_s::unbind();

            framebuffer_->get_texture()->generate_mip_maps();
        }

        if (framebuffer_) {
            iface_tex_.set_value(framebuffer_->get_texture());
        }
    }

    void complete(core::app_state_s& /*app*/) final
    {
        if (processed_frame_ && allocator_ && texture_) {
            allocator_->end_texture_use(*texture_);
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

    bool test_option(std::string_view name, const nlohmann::json& value) const final
    {
        if (name == "device_name") {
            return value.is_string();
        }

        return false;
    }

    std::string_view type() const final { return "decklink_input"; }
};

HRESULT
callback_s::VideoInputFrameArrived(IDeckLinkVideoInputFrame* videoFrame, IDeckLinkAudioInputPacket* audioPacket)
{
    auto node = node_.lock();
    if (!node || videoFrame == nullptr) {
        return S_OK;
    }

    auto lock = node->allocator_ctx_->get_lock();

    void* ptr{};
    videoFrame->GetBytes(&ptr);

    auto* transfer = node->allocator_->get_transfer(ptr);
    if (transfer == nullptr) {
        return S_OK;
    }

    node->allocator_ctx_->make_current();

    {
        std::unique_lock lock(node->frame_mutex_);
        if (node->frame_queue_.size() < 2) {
            transfer->perform_copy();
            node->frame_queue_.emplace(decklink_ptr<IDeckLinkVideoFrame>::make_owner(videoFrame), transfer);
        }
    }

    gpu::context_s::rewind_current();
    return S_OK;
}

HRESULT
callback_s::VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                                    IDeckLinkDisplayMode*            newDisplayMode,
                                    BMDDetectedVideoInputFormatFlags detectedSignalFlags)
{
    auto node = node_.lock();
    if (!node || newDisplayMode == nullptr) {
        return S_OK;
    }

    std::unique_lock lock(node->frame_mutex_);

    if (node->device_) {
        node->device_->PauseStreams();
        node->device_->FlushStreams();
        node->device_->EnableVideoInput(
            newDisplayMode->GetDisplayMode(), bmdFormat8BitYUV, bmdVideoInputEnableFormatDetection);
        node->device_->StartStreams();
    }

    return S_OK;
}

std::shared_ptr<node_i> create_input_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::decklink
