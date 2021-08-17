#include "core/app_state.hpp"
#include "detail/frame.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/sync.hpp"
#include "gpu/texture.hpp"
#include "gpu/types.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/validate_option.hpp"
#include "registry.hpp"
#include "utils/frame_queue.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <memory>
#include <optional>
#include <unordered_set>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace miximus::nodes::decklink;
using namespace std::chrono_literals;

using texture_ptr_t = std::unique_ptr<gpu::texture_s>;

struct frame_info_s
{
    GLuint                       buffer_id{};
    std::unique_ptr<gpu::sync_s> sync;
    gpu::vec2i_t                 dim{};

    frame_info_s() = default;
    ~frame_info_s()
    {
        if (buffer_id != 0) {
            glDeleteBuffers(1, &buffer_id); // Deleting 0 is safe
        }
    }

    frame_info_s(const frame_info_s&) = delete;
    frame_info_s(frame_info_s&& o) noexcept { *this = std::move(o); }
    frame_info_s& operator=(const frame_info_s&) = delete;
    frame_info_s& operator                       =(frame_info_s&& o) noexcept
    {
        buffer_id   = o.buffer_id;
        o.buffer_id = 0;
        sync        = std::move(o.sync);
        dim         = o.dim;

        return *this;
    }
};

class node_impl;

auto log() { return getlog("decklink"); }

class callback_s : public IDeckLinkInputCallback
{
    std::atomic_ulong                      ref_count_{1};
    std::shared_ptr<gpu::context_s>        ctx_;
    decklink_ptr<IDeckLinkVideoConversion> converter_;

    utils::frame_queue_s<frame_info_s> frames_rendered_;
    utils::frame_queue_s<frame_info_s> frames_free_;

    std::atomic<BMDDisplayMode>    new_display_mode_{bmdModeUnknown};
    std::atomic<BMDFieldDominance> frame_field_dominance_{bmdUnknownFieldDominance};

  public:
    explicit callback_s(std::shared_ptr<gpu::context_s> ctx)
        : ctx_(std::move(ctx))
        , converter_(decklink_registry_s::get_converter())
    {
        for (int i = 0; i < 5; i++) {
            frames_free_.push_frame({});
        }
    }

    ~callback_s() override
    {
        auto lock = ctx_->get_lock();
        ctx_->make_current();

        frames_rendered_.clear();
        frames_free_.clear();

        gpu::context_s::rewind_current();
    }

    callback_s(const callback_s&) = delete;
    callback_s(callback_s&&)      = delete;
    callback_s& operator=(const callback_s&) = delete;
    callback_s& operator=(callback_s&&) = delete;

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
        auto frame_arrival_time = utils::flicks_now();

        if (videoFrame == nullptr) {
            return S_OK;
        }

        auto [record, _] = frames_free_.pop_frame();
        if (!record) {
            log()->warn("VideoInputFrameArrived dropped frame");
            return S_OK;
        }

        auto lock = ctx_->get_lock();
        ctx_->make_current();

        auto&        frame = record->frame;
        gpu::vec2i_t dim{videoFrame->GetWidth(), videoFrame->GetHeight()};

        if (frame.buffer_id == 0 || frame.dim != dim) {
            glDeleteBuffers(1, &frame.buffer_id); // Deleting 0 is safe
            glCreateBuffers(1, &frame.buffer_id);
            glNamedBufferStorage(frame.buffer_id,
                                 dim.x * dim.y * 4,
                                 nullptr,
                                 GLbitfield(GL_DYNAMIC_STORAGE_BIT) | GLbitfield(GL_MAP_WRITE_BIT));
            frame.dim = dim;
        }

        {
            // Map the buffer to client memory address space
            void* data = glMapNamedBuffer(frame.buffer_id, GL_WRITE_ONLY);

            // Wrap the buffer ptr as a IDeckLinkVideoFrame
            auto dst_frame =
                make_decklink_ptr<detail::decklink_frame_s>(data, dim.x, dim.y, dim.x * 4, bmdFormat10BitRGBXLE);

            // Convert the frame directly into the buffer
            if (FAILED(converter_->ConvertFrame(videoFrame, dst_frame.get()))) {
                assert(false);
            }

            glUnmapNamedBuffer(frame.buffer_id);
        }

        frame.sync = std::make_unique<gpu::sync_s>();

        frames_rendered_.push_frame(std::move(frame), frame_arrival_time);

        gpu::context_s::rewind_current();
        return S_OK;
    }

    HRESULT
    VideoInputFormatChanged(BMDVideoInputFormatChangedEvents notificationEvents,
                            IDeckLinkDisplayMode*            newDisplayMode,
                            BMDDetectedVideoInputFormatFlags /*detectedSignalFlags*/) final
    {
        if ((notificationEvents & bmdVideoInputDisplayModeChanged) != 0) {
            new_display_mode_      = newDisplayMode->GetDisplayMode();
            frame_field_dominance_ = newDisplayMode->GetFieldDominance();
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
};

class node_impl : public node_i
{
    static inline std::unordered_set<IDeckLinkInput*> devices_in_use;
    decklink_ptr<IDeckLinkInput>                      device_;
    decklink_ptr<callback_s>                          callback_;

    std::unique_ptr<gpu::texture_s>     texture_;
    std::unique_ptr<gpu::framebuffer_s> framebuffer_;
    std::unique_ptr<gpu::draw_state_s>  draw_state_;
    std::optional<frame_info_s>         work_frame_;

    output_interface_s<gpu::texture_s*> iface_tex_{"tex"};

    void free_device()
    {
        devices_in_use.erase(device_.get());

        device_->StopStreams();
        device_->DisableAudioInput();
        device_->DisableVideoInput();
        device_->FlushStreams();

        device_   = nullptr;
        callback_ = nullptr;
        texture_.reset();
        framebuffer_.reset();
        work_frame_ = std::nullopt;
    }

  public:
    explicit node_impl() { iface_tex_.register_interface(&interfaces_); }

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

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* /*traits*/) final
    {
        if (device_ && callback_) {
            auto pts    = app->frame_info.timestamp;
            auto flush  = pts - app->frame_info.duration * 2;
            work_frame_ = callback_->get_rendered_frame(pts, flush);

            auto new_mode = callback_->get_new_display_mode();
            if (new_mode != bmdModeUnknown) {
                // TODO(Love): Error checking of DeckLink calls
                device_->PauseStreams();
                device_->EnableVideoInput(new_mode, bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection);
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

            if (!device || devices_in_use.count(device.get()) > 0) {
                return;
            }

            log()->info("Setting up DeckLink input {}", device_name);
            device_ = device;
            devices_in_use.emplace(device_.get());

            auto ctx = gpu::context_s::create_shared_context(false, app->ctx());

            callback_ = make_decklink_ptr<callback_s>(ctx);
            device_->SetCallback(callback_.get());

            // TODO(Love): Error checking of DeckLink calls
            device_->EnableVideoInput(bmdModeNTSC, bmdFormat10BitYUV, bmdVideoInputEnableFormatDetection);
            device_->StartStreams();
        }
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final
    {
        if (!work_frame_) {
            iface_tex_.set_value(framebuffer_ ? framebuffer_->texture() : nullptr);
            return;
        }

        if (!draw_state_) {
            draw_state_  = std::make_unique<gpu::draw_state_s>();
            auto* shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::strip_gamma);
            draw_state_->set_shader_program(shader);
            draw_state_->set_vertex_data(gpu::full_screen_quad_verts_flip_uv);
        }

        auto dim = work_frame_->dim;

        if (!texture_ || !framebuffer_ || texture_->texture_dimensions() != dim) {
            framebuffer_ = std::make_unique<gpu::framebuffer_s>(dim, gpu::texture_s::format_e::rgb_f16);
            texture_     = std::make_unique<gpu::texture_s>(dim, gpu::texture_s::format_e::rgb_f16);
        }

        assert(work_frame_->sync);
        if (work_frame_->sync) {
            work_frame_->sync->gpu_wait();
        }

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, work_frame_->buffer_id);
        glTextureSubImage2D(texture_->id(), 0, 0, 0, dim.x, dim.y, GL_BGRA, GL_UNSIGNED_INT_10_10_10_2, nullptr);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);

        auto* shader = draw_state_->get_shader_program();
        shader->set_uniform("offset", gpu::vec2i_t{0, 0});
        shader->set_uniform("scale", gpu::vec2i_t{1.0, 1.0});

        glViewport(0, 0, dim.x, dim.y);
        glClearColor(0, 0, 0, 0);
        glClear(GLbitfield(GL_COLOR_BUFFER_BIT) | GLbitfield(GL_DEPTH_BUFFER_BIT));

        framebuffer_->bind();
        texture_->bind(0);
        draw_state_->draw();
        gpu::texture_s::unbind(0);
        gpu::framebuffer_s::unbind();

        auto* fb_tex = framebuffer_->texture();
        fb_tex->generate_mip_maps();
        iface_tex_.set_value(fb_tex);
    }

    void complete(core::app_state_s* /*app*/) final
    {
        if (work_frame_ && callback_) {
            work_frame_->sync.reset();
            callback_->push_free_frame(std::move(*work_frame_));
        }

        work_frame_ = std::nullopt;
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
