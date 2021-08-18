#include "core/app_state.hpp"
#include "detail/frame.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/glad.hpp"
#include "gpu/sync.hpp"
#include "gpu/texture.hpp"
#include "logger/logger.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/validate_option.hpp"
#include "registry.hpp"
#include "utils/frame_queue.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"

#include <memory>
#include <optional>
#include <queue>
#include <unordered_set>

namespace {
using namespace miximus;
using namespace miximus::nodes;
using namespace miximus::nodes::decklink;
using namespace std::chrono_literals;

class node_impl;

auto log() { return getlog("decklink"); }

struct mode_info_s
{
    BMDDisplayMode mode;
    BMDTimeValue   frame_duration;
    BMDTimeScale   time_scale;
    gpu::vec2i_t   dim;
};

struct frame_info_s
{
    GLuint                       buffer_id{};
    std::unique_ptr<gpu::sync_s> sync;
    gpu::vec2i_t                 dim{};
    void*                        ptr{};

    frame_info_s() = default;
    ~frame_info_s()
    {
        if (ptr != nullptr) {
            glUnmapNamedBuffer(buffer_id);
        }

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
        ptr         = o.ptr;
        o.ptr       = nullptr;

        return *this;
    }
};

class callback_s
    : public IDeckLinkVideoOutputCallback
    , public IDeckLinkAudioOutputCallback
{
    std::atomic_ulong ref_count_{1};

    std::shared_ptr<gpu::context_s>        ctx_;
    utils::frame_queue_s<frame_info_s>     frames_rendered_;
    utils::frame_queue_s<frame_info_s>     frames_free_;
    decklink_ptr<IDeckLinkVideoFrame>      last_frame_;
    decklink_ptr<IDeckLinkVideoConversion> converter_;

    IDeckLinkOutput* const device_;
    const mode_info_s      mode_info_;
    BMDTimeValue           pts_{0};

  public:
    callback_s(const std::shared_ptr<gpu::context_s>& ctx, IDeckLinkOutput* device, mode_info_s mode_info)
        : ctx_(ctx)
        , device_(device)
        , mode_info_(mode_info)
        , converter_(decklink_registry_s::get_converter())
    {
        auto lock = ctx_->get_lock();
        auto dim  = mode_info_.dim;

        IDeckLinkMutableVideoFrame* frame = nullptr;

        if (SUCCEEDED(device_->CreateVideoFrame(
                mode_info_.dim.x, mode_info_.dim.y, mode_info.dim.x * 2, bmdFormat8BitYUV, 0, &frame))) {
            uint16_t* data = nullptr;
            frame->GetBytes(reinterpret_cast<void**>(&data));

            std::fill(data, data + mode_info_.dim.x * mode_info_.dim.y, 0x0000);

            for (int i = 0; i < 4; i++) {
                device_->ScheduleVideoFrame(frame, pts_, mode_info_.frame_duration, mode_info_.time_scale);
            }

            last_frame_ = frame;
            frame->Release();
        }

        for (int i = 0; i < 7; i++) {
            frames_free_.push_frame({});
        }
    }

    ~callback_s()
    {
        auto lock = ctx_->get_lock();
        ctx_->make_current();

        frames_free_.clear();
        frames_rendered_.clear();

        gpu::context_s::rewind_current();
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
     * IDeckLinkVideoOutputCallback
     */
    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame* /*completedFrame*/,
                                                      BMDOutputFrameCompletionResult result) final
    {
        if (result == bmdOutputFrameFlushed) {
            return S_OK;
        }

        auto lock = ctx_->get_lock();
        ctx_->make_current();

        if (auto [record, size] = frames_rendered_.pop_frame_if_count(3); record != std::nullopt) {
            auto& frame = record->frame;

            if (frame.sync) {
                frame.sync->gpu_wait();
                frame.sync.reset();
            }

            glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
            glFinish();

            IDeckLinkMutableVideoFrame* dst_frame = nullptr;
            // int row_bytes = ((frame.dim.x + 47) / 48) * 128;
            int row_bytes = frame.dim.x * 2;

            if (SUCCEEDED(
                    device_->CreateVideoFrame(frame.dim.x, frame.dim.y, row_bytes, bmdFormat8BitYUV, 0, &dst_frame))) {
                auto src_frame = make_decklink_ptr<detail::decklink_frame_s>(
                    frame.ptr, frame.dim.x, frame.dim.y, frame.dim.x * 4, bmdFormat8BitBGRA);
                converter_->ConvertFrame(src_frame.get(), dst_frame);

                last_frame_ = dst_frame;
                dst_frame->Release();
            }

            frame.sync = std::make_unique<gpu::sync_s>();
            frames_free_.push_frame(std::move(frame));
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

        gpu::context_s::rewind_current();

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

    void push_rendered_frame(frame_info_s&& frame) { frames_rendered_.push_frame(std::move(frame)); }

    std::optional<frame_info_s> get_free_frame()
    {
        auto [record, _] = frames_free_.pop_frame();
        if (record) {
            return std::move(record->frame);
        }

        return std::nullopt;
    }
};

class node_impl : public node_i
{
    static inline std::unordered_set<IDeckLinkOutput*> devices_in_use;
    decklink_ptr<IDeckLinkOutput>                      device_;
    decklink_ptr<callback_s>                           callback_;
    std::map<std::string, mode_info_s>                 display_modes_;

    std::unique_ptr<gpu::framebuffer_s> framebuffer_;
    std::unique_ptr<gpu::draw_state_s>  draw_state_;
    std::string                         display_mode_str_;
    mode_info_s*                        display_mode_{};
    // gpu::color_transfer_e               color_transfer_ = gpu::color_transfer_e::Rec601;

    input_interface_s<gpu::texture_s*> iface_tex_{"tex"};

    void free_device()
    {
        devices_in_use.erase(device_.get());
        device_->StopScheduledPlayback(0, nullptr, 1.0);

        device_   = nullptr;
        callback_ = nullptr;
        framebuffer_.reset();
        display_modes_.clear();
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

    void stop_playback()
    {
        callback_ = nullptr;

        if (!device_) {
            return;
        }

        auto res = device_->StopScheduledPlayback(0, nullptr, 0);
        // assert(res == S_OK);

        res = device_->DisableVideoOutput();
        // assert(res == S_OK);

        res = device_->DisableAudioOutput();
        // assert(res == S_OK);

        res = device_->FlushBufferedAudioSamples();
        // assert(res == S_OK);
    }

    void restart_playback(core::app_state_s* app)
    {
        assert(device_);
        stop_playback();

        auto mode_it = display_modes_.find(display_mode_str_);
        if (mode_it == display_modes_.end()) {
            return;
        }

        display_mode_ = &mode_it->second;

        log()->info("Enabling video output with {}", display_mode_str_);

        auto res = device_->EnableVideoOutput(display_mode_->mode, bmdVideoOutputFlagDefault);
        assert(res == S_OK);

        auto ctx  = gpu::context_s::create_shared_context(false, app->ctx());
        callback_ = make_decklink_ptr<callback_s>(ctx, device_.get(), *display_mode_);
        res       = device_->SetScheduledFrameCompletionCallback(callback_.get());
        assert(res == S_OK);

        res = device_->SetAudioCallback(callback_.get());
        assert(res == S_OK);

        res = device_->StartScheduledPlayback(0, display_mode_->time_scale, 1.0);
        assert(res == S_OK);
    }

    void prepare(core::app_state_s* app, const node_state_s& state, traits_s* traits) final
    {
        traits->must_run = true;

        auto device_name  = state.get_option<std::string>("device_name");
        auto display_mode = state.get_option<std::string>("display_mode");
        auto enabled      = state.get_option<bool>("enabled");

        auto device = enabled ? app->decklink_registry()->get_output(device_name) : nullptr;
        if (device == device_) {
            // Check if the display_mode setting has changed since last frame
            if (display_mode_str_ != display_mode) {
                display_mode_str_ = display_mode;
                restart_playback(app);
            }
        } else {
            if (device_) {
                free_device();
            }

            if (!device || devices_in_use.count(device.get()) > 0) {
                return;
            }

            log()->info("Setting up DeckLink output {}", device_name);
            device_ = device;
            devices_in_use.emplace(device_.get());

            IDeckLinkDisplayModeIterator* itr   = nullptr;
            IDeckLinkDisplayMode*         imode = nullptr;

            {
                device_->GetDisplayModeIterator(&itr);

                while (itr->Next(&imode) == S_OK) {
                    mode_info_s mode{};

                    mode.mode = imode->GetDisplayMode();
                    mode.dim  = {imode->GetWidth(), imode->GetHeight()};
                    imode->GetFrameRate(&mode.frame_duration, &mode.time_scale);

                    auto name = decklink_registry_s::get_display_mode_name(imode);
                    display_modes_.emplace(name, mode);

                    imode->Release();
                }

                itr->Release();
            }

            display_mode_str_ = display_mode;
            restart_playback(app);
        }
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto* texture = iface_tex_.resolve_value(app, nodes, state);
        if (texture == nullptr) {
            return;
        }

        if (!callback_ || !device_) {
            return;
        }

        auto frame = callback_->get_free_frame();
        if (!frame) {
            return;
        }

        if (!draw_state_) {
            draw_state_  = std::make_unique<gpu::draw_state_s>();
            auto* shader = app->ctx()->get_shader(gpu::shader_program_s::name_e::apply_gamma);
            draw_state_->set_shader_program(shader);
            draw_state_->set_vertex_data(gpu::full_screen_quad_verts_flip_uv);
        }

        auto dim = display_mode_->dim;

        if (!framebuffer_ || framebuffer_->texture()->texture_dimensions() != dim) {
            framebuffer_ = std::make_unique<gpu::framebuffer_s>(dim, gpu::texture_s::format_e::bgra_u8);
        }

        if (frame->buffer_id == 0 || frame->dim != dim) {
            auto buffer_size = dim.x * dim.y * 4;
            frame->sync.reset();
            if (frame->ptr) {
                glUnmapNamedBuffer(frame->buffer_id);
            }

            glDeleteBuffers(1, &frame->buffer_id);
            glCreateBuffers(1, &frame->buffer_id);

            glNamedBufferStorage(frame->buffer_id,
                                 buffer_size,
                                 nullptr,
                                 GL_MAP_READ_BIT | GL_DYNAMIC_STORAGE_BIT | GL_MAP_PERSISTENT_BIT);
            frame->ptr =
                glMapNamedBufferRange(frame->buffer_id, 0, buffer_size, GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT);
            frame->dim = dim;
        }

        framebuffer_->bind();
        glViewport(0, 0, dim.x, dim.y);
        glClearColor(0, 0, 0, 0);
        glClear(GLbitfield(GL_COLOR_BUFFER_BIT) | GLbitfield(GL_DEPTH_BUFFER_BIT));

        auto* shader = draw_state_->get_shader_program();
        shader->set_uniform("offset", {0, 0});
        shader->set_uniform("scale", {1.0, 1.0});

        texture->bind(0);
        draw_state_->draw();
        gpu::texture_s::unbind(0);

        glBindBuffer(GL_PIXEL_PACK_BUFFER, frame->buffer_id);
        glReadPixels(0, 0, dim.x, dim.y, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, nullptr);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        gpu::framebuffer_s::unbind();

        frame->sync = std::make_unique<gpu::sync_s>();

        callback_->push_rendered_frame(std::move(*frame));
    }

    void complete(core::app_state_s* /*app*/) final {}

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "DeckLink input"},
            {"enabled", true},
            {"display_mode", "720p60"},
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "device_name" || name == "display_mode") {
            return validate_option<std::string_view>(value);
        }

        if (name == "enabled") {
            return validate_option<bool>(value);
        }

        return false;
    }

    std::string_view type() const final { return "decklink_output"; }
};
} // namespace

namespace miximus::nodes::decklink {
std::shared_ptr<node_i> create_output_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::decklink
