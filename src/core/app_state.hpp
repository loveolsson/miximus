#pragma once
#include "core/command_line_options.hpp"
#include "core/frame_context.hpp"
#include "core/node_status_registry_fwd.hpp"
#include "gpu/completion.hpp"
#include "gpu/texture_fwd.hpp"
#include "gpu/transfer/texture_readback_fwd.hpp"
#include "gpu/transfer/texture_upload_fwd.hpp"
#include "gpu/types.hpp"
#include "gpu/window_fwd.hpp"
#include "nodes/cef/subsystem_fwd.hpp"
#include "nodes/decklink/registry_fwd.hpp"
#include "nodes/execution_context.hpp"
#include "nodes/ffmpeg/registry_fwd.hpp"
#include "nodes/frame_commands.hpp"
#include "nodes/ndi/registry_fwd.hpp"
#include "render/font/font_registry_fwd.hpp"
#include "types/frame_rate.hpp"
#include "utils/file_dialog.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace boost::asio {
class io_context;
}

namespace miximus::gpu {
class device_s;
class recording_s;
} // namespace miximus::gpu

namespace miximus::utils {
class cpu_task_worker_s;
}

namespace miximus::core {

class app_state_s
{
  public:
    using frame_settings_s = nodes::frame_settings_s;

  private:
    struct configuration_worker_s;
    const command_line_options_s              command_line_options_;
    std::unique_ptr<configuration_worker_s>   configuration_worker_;
    std::unique_ptr<utils::cpu_task_worker_s> cpu_task_worker_;
    std::unique_ptr<utils::file_dialog_s>     file_dialog_;

    std::unique_ptr<gpu::window_system_s>                      window_system_;
    std::unique_ptr<gpu::device_s>                             gpu_;
    std::unique_ptr<nodes::frame_commands_s>                   frame_commands_;
    std::unique_ptr<gpu::texture_s>                            fallback_texture_;
    std::unique_ptr<gpu::transfer::texture_upload_service_s>   texture_upload_service_;
    std::unique_ptr<gpu::transfer::texture_readback_service_s> texture_readback_service_;
    std::unique_ptr<nodes::decklink::decklink_registry_s>      decklink_registry_;
    std::unique_ptr<nodes::ffmpeg::ffmpeg_registry_s>          ffmpeg_registry_;
    std::unique_ptr<nodes::ndi::ndi_registry_s>                ndi_registry_;
    std::unique_ptr<render::font_registry_s>                   font_registry_;
    std::unique_ptr<node_status_registry_s>                    status_registry_;
    std::shared_ptr<nodes::cef::subsystem_s>                   cef_subsystem_;
    std::string                                                cef_error_{"CEF support is not enabled in this build"};

    frame_settings_s frame_settings_{};
    frame_context_s  frame_context_{};

  public:
    // Builds only frame-local state so graph lifecycle tests do not initialize
    // hardware, worker threads, or GPU resources.
    struct test_state_t
    {
        explicit test_state_t() = default;
    };

    app_state_s();
    explicit app_state_s(command_line_options_s command_line_options);
    explicit app_state_s(test_state_t, command_line_options_s command_line_options = {});
    ~app_state_s();

    boost::asio::io_context* cfg_executor() noexcept;

    auto gpu() noexcept { return gpu_.get(); }

    gpu::recording_s& commands();
    gpu::completion_s submit_gpu();
    void              defer_output(std::function<void(gpu::completion_s)> publish);
    void              commit_gpu_frame();

    // Always cancel uncommitted output leases on stack rewind. Earlier GPU
    // submissions remain valid, but cannot publish a partially evaluated frame.
    class frame_scope_s
    {
        app_state_s& app_;

      public:
        explicit frame_scope_s(app_state_s& app)
            : app_(app)
        {
        }

        ~frame_scope_s() { app_.abort_gpu(); }

        frame_scope_s(const frame_scope_s&)            = delete;
        frame_scope_s& operator=(const frame_scope_s&) = delete;
    };

    void abort_gpu() noexcept;

    auto fallback_texture() noexcept { return fallback_texture_.get(); }

    auto texture_upload_service() noexcept { return texture_upload_service_.get(); }

    auto texture_readback_service() noexcept { return texture_readback_service_.get(); }

    auto decklink_registry() noexcept { return decklink_registry_.get(); }

    auto ndi_registry() noexcept { return ndi_registry_.get(); }

    auto font_registry() noexcept { return font_registry_.get(); }

    auto cpu_task_worker() noexcept { return cpu_task_worker_.get(); }

    auto file_dialog() noexcept { return file_dialog_.get(); }

    auto status_registry() noexcept { return status_registry_.get(); }

    auto ffmpeg_registry() noexcept { return ffmpeg_registry_.get(); }

    nodes::cef::subsystem_s* cef_subsystem() noexcept { return cef_subsystem_.get(); }

    const std::string& cef_error() const noexcept { return cef_error_; }

    const command_line_options_s& command_line_options() const noexcept { return command_line_options_; }

    void begin_frame(frame_settings_s settings, frame_context_s frame_context) noexcept;

    const frame_settings_s& frame_settings() const noexcept { return frame_settings_; }

    const frame_context_s& frame_context() const noexcept { return frame_context_; }

    nodes::frame_execution_state_s frame_info;

    nodes::execution_context_s     node_context();
    nodes::configuration_context_s configuration_context();
};

} // namespace miximus::core
