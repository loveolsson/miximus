#include "core/app_state.hpp"
#include "detail/frame.hpp"
#include "gpu/color_transfer.hpp"
#include "gpu/context.hpp"
#include "gpu/draw_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/glad.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/transfer.hpp"
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
using frame_info_t  = std::pair<std::unique_ptr<gpu::framebuffer_s>, std::unique_ptr<gpu::transfer::transfer_i>>;

class node_impl;

auto log() { return getlog("decklink"); }

class callback_s
    : public IDeckLinkVideoOutputCallback
    , public IDeckLinkAudioOutputCallback
{
    std::atomic_ulong               ref_count_{1};
    std::shared_ptr<gpu::context_s> ctx_;

    std::mutex                    frame_mtx_;
    std::queue<frame_info_t>      frames_rendered_;
    std::queue<frame_info_t>      frames_free_;
    std::map<void*, frame_info_t> frames_queued_;

  public:
    callback_s(std::shared_ptr<gpu::context_s> ctx)
        : ctx_(std::move(ctx))
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
     * IDeckLinkVideoOutputCallback
     */
    HRESULT ScheduledFrameCompleted(IDeckLinkVideoFrame* completedFrame, BMDOutputFrameCompletionResult result) final
    {
        std::unique_lock lock(frame_mtx_);

        void* data = nullptr;
        completedFrame->GetBytes(&data);

        auto it = frames_queued_.find(data);
        if (it != frames_queued_.end()) {
            frames_free_.emplace(std::move(it->second));
            frames_queued_.erase(it);
        }

        return S_OK;
    }

    HRESULT ScheduledPlaybackHasStopped() final { return S_OK; }

    /**
     * IDeckLinkAudioOutputCallback
     */
    HRESULT RenderAudioSamples(bool preroll) final { return S_OK; }
};

class node_impl : public node_i
{
    static inline std::unordered_set<IDeckLinkOutput*> devices_in_use;
    decklink_ptr<IDeckLinkOutput>                      device_;
    decklink_ptr<callback_s>                           callback_;
    std::map<std::string, BMDDisplayMode>              display_modes_;

    std::unique_ptr<gpu::framebuffer_s> framebuffer_;
    std::unique_ptr<gpu::draw_state_s>  draw_state_;
    // gpu::color_transfer_e               color_transfer_ = gpu::color_transfer_e::Rec601;

    input_interface_s<gpu::texture_s*> iface_tex_;

    void free_device()
    {
        devices_in_use.erase(device_.ptr());
        device_->StopScheduledPlayback(0, nullptr, 1.0);

        if (framebuffer_) {
            auto type = gpu::transfer::transfer_i::get_prefered_type();
            gpu::transfer::transfer_i::unregister_texture(type, framebuffer_->texture());
        }

        device_   = nullptr;
        callback_ = nullptr;
        framebuffer_.reset();
        display_modes_.clear();
        // color_transfer_ = gpu::color_transfer_e::Rec601;
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

        auto device_name  = state.get_option<std::string>("device_name");
        auto display_mode = state.get_option<std::string>("display_mode");
        auto enabled      = state.get_option<bool>("enabled");

        auto device = enabled ? app->decklink_registry()->get_output(device_name) : nullptr;
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

        log()->info("Setting up DeckLink output {}", device_name);
        device_ = device;
        devices_in_use.emplace(device_.ptr());

        auto ctx = gpu::context_s::create_shared_context(false, app->ctx());

        callback_ = new callback_s(ctx);
        device_->SetScheduledFrameCompletionCallback(callback_.ptr());
        device_->SetAudioCallback(callback_.ptr());

        IDeckLinkDisplayModeIterator* itr   = nullptr;
        IDeckLinkDisplayMode*         imode = nullptr;

        {
            device_->GetDisplayModeIterator(&itr);

            while (itr->Next(&imode) == S_OK) {
                auto name = decklink_registry_s::get_display_mode_name(imode);
                auto mode = imode->GetDisplayMode();
                display_modes_.emplace(name, mode);
                imode->Release();
            }

            itr->Release();
        }

        auto mode_it = display_modes_.find(display_mode);
        if (mode_it != display_modes_.end()) {
        }
    }

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& /*state*/) final {}

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
