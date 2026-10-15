#include "app_state.hpp"

#include "core/node_status_registry.hpp"
#include "gpu/device.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_readback.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "gpu/window.hpp"
#include "logger/logger.hpp"
#include "utils/asio.hpp"
#include "utils/cpu_task_worker.hpp"
#include "utils/environment.hpp"
#include "utils/failure_shutdown.hpp"

#if MIXIMUS_ENABLE_CEF
#include "nodes/cef/subsystem.hpp"
#endif
#include "nodes/decklink/registry.hpp"
#include "nodes/ffmpeg/registry.hpp"
#include "nodes/ndi/registry.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"
#include "utils/shutdown_watchdog.hpp"

#include <cstdlib>
#include <exception>
#include <memory>
#include <thread>
#include <utility>

using namespace boost::asio;

namespace miximus::core {
struct app_state_s::configuration_worker_s
{
    boost::asio::io_context                                                                   executor;
    std::unique_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>> work;
    std::thread                                                                               thread;
};

boost::asio::io_context* app_state_s::cfg_executor() noexcept { return &configuration_worker_->executor; }

namespace {

gpu::device_options_s gpu_options(const command_line_options_s& command_line)
{
    gpu::device_options_s options;
    options.use_cuda       = command_line.use_cuda;
    options.presentation   = true;
    options.max_recordings = 32;
#if MIXIMUS_ENABLE_CEF || defined(_WIN32)
    // Windows playback imports D3D11 surfaces even without browser nodes.
    options.external_image_import = true;
#endif
    // Startup reads only: the app does not mutate the process environment.
    if (const auto uuid = miximus::utils::environment_value("MIXIMUS_VULKAN_DEVICE_UUID")) {
        options.device_uuid = *uuid;
    }

    if (const auto validation = miximus::utils::environment_value("MIXIMUS_VULKAN_VALIDATION")) {
        options.validation = std::string_view(*validation) == "1";
    }

    return options;
}
} // namespace

gpu::recording_s& app_state_s::commands() { return frame_commands_->commands(); }

gpu::completion_s app_state_s::submit_gpu() { return frame_commands_->submit_gpu(); }

void app_state_s::defer_output(std::function<void(gpu::completion_s)> publish)
{
    frame_commands_->defer_output(std::move(publish));
}

void app_state_s::commit_gpu_frame() { frame_commands_->commit(); }

void app_state_s::abort_gpu() noexcept { frame_commands_->abort(); }

namespace {
constexpr int FALLBACK_TEXTURE_DIMENSION = 16;
}

nodes::execution_context_s app_state_s::node_context()
{
    return {
        {.device        = gpu_.get(),
         .fallback      = fallback_texture_.get(),
         .uploads       = texture_upload_service_.get(),
         .readbacks     = texture_readback_service_.get(),
         .decklink      = decklink_registry_.get(),
         .ndi           = ndi_registry_.get(),
         .ffmpeg        = ffmpeg_registry_.get(),
         .fonts         = font_registry_.get(),
         .cpu_tasks     = cpu_task_worker_.get(),
         .status        = status_registry_.get(),
         .browser       = cef_subsystem_.get(),
         .browser_error = &cef_error_},
        frame_settings_,
        frame_context_,
        *frame_commands_,
        frame_info
    };
}

nodes::configuration_context_s app_state_s::configuration_context()
{
    return {.browser = cef_subsystem_.get(), .browser_error = &cef_error_};
}

void app_state_s::begin_frame(frame_settings_s settings, frame_context_s frame_context) noexcept
{
    frame_settings_ = settings;
    frame_context_  = frame_context;
}

app_state_s::app_state_s()
    : app_state_s(command_line_options_s{})
{
}

app_state_s::app_state_s(command_line_options_s command_line_options)
    : command_line_options_(std::move(command_line_options))
    , configuration_worker_(std::make_unique<configuration_worker_s>())
    , cpu_task_worker_(std::make_unique<utils::cpu_task_worker_s>())
    , file_dialog_(std::make_unique<utils::file_dialog_s>())
    , window_system_(std::make_unique<gpu::window_system_s>())
    , gpu_(std::make_unique<gpu::device_s>(gpu_options(command_line_options_)))
    , frame_commands_(std::make_unique<nodes::frame_commands_s>(gpu_.get()))
    , decklink_registry_(nodes::decklink::decklink_registry_s::create_decklink_registry())
    , ndi_registry_(nodes::ndi::ndi_registry_s::create_ndi_registry())
    , font_registry_(render::font_registry_s::create_font_registry())
    , status_registry_(std::make_unique<node_status_registry_s>(configuration_worker_->executor))
{
    configuration_worker_->work = std::make_unique<executor_work_guard<io_context::executor_type>>(
        make_work_guard(configuration_worker_->executor));
    fallback_texture_ = std::make_unique<gpu::texture_s>(
        *gpu_, gpu::vec2i_t{FALLBACK_TEXTURE_DIMENSION, FALLBACK_TEXTURE_DIMENSION}, gpu::format_e::rgba_unorm16);
    fallback_texture_->clear(commands());
    submit_gpu();
    texture_upload_service_   = std::make_unique<gpu::transfer::texture_upload_service_s>(*gpu_);
    texture_readback_service_ = std::make_unique<gpu::transfer::texture_readback_service_s>(*gpu_);
    ffmpeg_registry_          = std::make_unique<nodes::ffmpeg::ffmpeg_registry_s>(*gpu_, *texture_upload_service_);
#if MIXIMUS_ENABLE_CEF
    try {
        auto profile = command_line_options_.settings_path;
        profile += ".cef";
        cef_subsystem_ = std::make_shared<nodes::cef::subsystem_s>(
            *gpu_,
            profile,
            nodes::cef::runtime_options_s{.disable_sandbox = command_line_options_.disable_cef_sandbox});
        cef_error_.clear();
    } catch (const std::exception& failure) {
        cef_error_ = failure.what();
        getlog("app")->error("CEF browser subsystem unavailable: {}", cef_error_);
    }
#endif
    configuration_worker_->thread = std::thread([this] {
        while (!configuration_worker_->executor.stopped()) {
            try {
                configuration_worker_->executor.run();
            } catch (...) {
                // Asio permits re-entering run() after a handler throws. Keep
                // servicing shutdown callbacks; new graph work is now rejected.
                utils::request_failure_shutdown("Configuration worker failed");
            }
        }
    });
}

app_state_s::app_state_s(test_state_t /*test_state*/, command_line_options_s command_line_options)
    : command_line_options_(std::move(command_line_options))
    , configuration_worker_(std::make_unique<configuration_worker_s>())
    , frame_commands_(std::make_unique<nodes::frame_commands_s>())
{
}

app_state_s::~app_state_s()
{
    try {
        if (std::uncaught_exceptions() != 0) {
            utils::start_shutdown_watchdog();
        }
        if (!window_system_) {
            return;
        }

        cpu_task_worker_->request_stop();
        file_dialog_->stop();
        decklink_registry_->uninstall();
        configuration_worker_->work = nullptr;

        // Capture-control work may still be retiring SDK buffers and upload
        // streams after a node was removed. Drain it before destroying the shared
        // transfer services it uses.
        utils::begin_shutdown_step("DeckLink subsystem");
        decklink_registry_.reset();
        utils::report_shutdown_step_completed();
        utils::begin_shutdown_step("NDI subsystem");
        ndi_registry_.reset();
        utils::report_shutdown_step_completed();

#if MIXIMUS_ENABLE_CEF
        utils::begin_shutdown_step("CEF subsystem");
        cef_subsystem_.reset();
        utils::report_shutdown_step_completed();
#endif
        utils::begin_shutdown_step("FFmpeg subsystem");
        ffmpeg_registry_.reset();
        utils::report_shutdown_step_completed();
        utils::begin_shutdown_step("CPU task worker");
        cpu_task_worker_.reset();
        utils::report_shutdown_step_completed();
        utils::begin_shutdown_step("GPU subsystem");
        abort_gpu();
        texture_readback_service_.reset();
        texture_upload_service_.reset();
        fallback_texture_.reset();
        frame_commands_.reset();
        gpu_.reset();
        window_system_.reset();
        utils::report_shutdown_step_completed();
        utils::begin_shutdown_step("application worker services");
        try {
            configuration_worker_->executor.stop();
        } catch (...) {
            utils::request_failure_shutdown("Failed to stop configuration worker");
        }
        configuration_worker_->thread.join();
    } catch (const std::exception& error) {
        utils::fail_without_unwinding("Application teardown failed", error.what());
    } catch (...) {
        utils::fail_without_unwinding("Application teardown failed");
    }
}

} // namespace miximus::core
