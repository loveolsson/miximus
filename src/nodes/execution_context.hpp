#pragma once
#include "core/frame_context.hpp"
#include "core/node_status_registry_fwd.hpp"
#include "execution_context_fwd.hpp"
#include "frame_execution_fwd.hpp"
#include "frame_settings.hpp"
#include "gpu/completion.hpp"
#include "gpu/texture_fwd.hpp"
#include "gpu/transfer/texture_readback_fwd.hpp"
#include "gpu/transfer/texture_upload_fwd.hpp"
#include "nodes/cef/subsystem_fwd.hpp"
#include "nodes/decklink/registry_fwd.hpp"
#include "nodes/ffmpeg/registry_fwd.hpp"
#include "nodes/ndi/registry_fwd.hpp"
#include "render/font/font_registry_fwd.hpp"

#include <functional>
#include <string>

namespace miximus::gpu {
class device_s;
class recording_s;
} // namespace miximus::gpu

namespace miximus::utils {
class cpu_task_worker_s;
}

namespace miximus::nodes {
class frame_commands_s;

// Borrowed only while dispatching a configuration action. No GPU, frame state,
// application ownership, or configuration executor is exposed to node handlers.
struct configuration_context_s
{
    cef::subsystem_s*  browser{};
    const std::string* browser_error{};

    cef::subsystem_s* cef_subsystem() const noexcept { return browser; }

    const std::string& cef_error() const;
};

struct execution_services_s
{
    gpu::device_s*                             device{};
    gpu::texture_s*                            fallback{};
    gpu::transfer::texture_upload_service_s*   uploads{};
    gpu::transfer::texture_readback_service_s* readbacks{};
    decklink::decklink_registry_s*             decklink{};
    ndi::ndi_registry_s*                       ndi{};
    ffmpeg::ffmpeg_registry_s*                 ffmpeg{};
    render::font_registry_s*                   fonts{};
    miximus::utils::cpu_task_worker_s*         cpu_tasks{};
    core::node_status_registry_s*              status{};
    cef::subsystem_s*                          browser{};
    const std::string*                         browser_error{};
};

struct frame_execution_state_s
{
    submitted_node_set_t submitted_nodes;
    executed_node_set_t  executed_nodes;
};

// A render-thread borrow, valid for this frame's callbacks only. The application
// owns every service. This exposes neither application lifecycle controls nor
// settings editing, frame commit, or the application's configuration executor.
class execution_context_s
{
    execution_services_s         services_;
    const frame_settings_s&      settings_;
    const core::frame_context_s& timing_;
    frame_commands_s&            commands_;

  public:
    frame_execution_state_s& frame_info;

    execution_context_s(execution_services_s         services,
                        const frame_settings_s&      settings,
                        const core::frame_context_s& timing,
                        frame_commands_s&            commands,
                        frame_execution_state_s&     state)
        : services_(services)
        , settings_(settings)
        , timing_(timing)
        , commands_(commands)
        , frame_info(state)
    {
    }

    const frame_settings_s& frame_settings() const noexcept { return settings_; }

    const core::frame_context_s& frame_context() const noexcept { return timing_; }

    gpu::recording_s& commands();
    gpu::completion_s submit_gpu();
    void              defer_output(std::function<void(gpu::completion_s)> publish);

    auto gpu() const noexcept { return services_.device; }

    auto fallback_texture() const noexcept { return services_.fallback; }

    auto texture_upload_service() const noexcept { return services_.uploads; }

    auto texture_readback_service() const noexcept { return services_.readbacks; }

    auto decklink_registry() const noexcept { return services_.decklink; }

    auto ndi_registry() const noexcept { return services_.ndi; }

    auto ffmpeg_registry() const noexcept { return services_.ffmpeg; }

    auto font_registry() const noexcept { return services_.fonts; }

    auto cpu_task_worker() const noexcept { return services_.cpu_tasks; }

    auto status_registry() const noexcept { return services_.status; }

    auto cef_subsystem() const noexcept { return services_.browser; }

    const std::string& cef_error() const;
};
} // namespace miximus::nodes
