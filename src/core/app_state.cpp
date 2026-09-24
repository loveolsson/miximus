#include "app_state.hpp"

#include "core/node_status_registry.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/texture_readback.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "gpu/window.hpp"
#if MIXIMUS_ENABLE_CEF
#include "logger/logger.hpp"
#include "nodes/cef/subsystem.hpp"
#endif
#include "nodes/decklink/registry.hpp"
#include "nodes/ndi/registry.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"
#include "utils/shutdown_watchdog.hpp"

#include <cstdlib>
#include <exception>
#include <memory>
#include <utility>

using namespace boost::asio;

namespace miximus::core {
namespace {

gpu::device_options_s gpu_options(const command_line_options_s& command_line)
{
    gpu::device_options_s options;
    options.use_cuda       = command_line.use_cuda;
    options.presentation   = true;
    options.max_recordings = 32;
#if MIXIMUS_ENABLE_CEF
    options.external_image_import = true;
#endif
    // Startup reads only: the app does not mutate the process environment.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const auto* uuid = std::getenv("MIXIMUS_VULKAN_DEVICE_UUID")) {
        options.device_uuid = uuid;
    }

    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const auto* validation = std::getenv("MIXIMUS_VULKAN_VALIDATION")) {
        options.validation = std::string_view(validation) == "1";
    }

    return options;
}
} // namespace

gpu::recording_s& app_state_s::commands()
{
    if (!recording_) {
        recording_ = gpu_->try_record();
        if (!recording_) {
            throw gpu::recording_unavailable_s{};
        }
    }

    return *recording_;
}

gpu::completion_s app_state_s::submit_gpu()
{
    if (recording_) {
        last_submission_ = recording_->submit();
        recording_.reset();
    }

    return last_submission_;
}

void app_state_s::defer_output(std::function<void(gpu::completion_s)> publish)
{
    pending_outputs_.emplace_back(std::move(publish));
}

void app_state_s::commit_gpu_frame()
{
    if (!pending_outputs_.empty()) {
        auto& recording = commands();
        for (auto& publish : pending_outputs_) {
            recording.on_submitted(std::move(publish));
        }
        pending_outputs_.clear();
    }
    submit_gpu();
}

void app_state_s::abort_gpu() noexcept
{
    recording_.reset();
    pending_outputs_.clear();
}
namespace {
constexpr int FALLBACK_TEXTURE_DIMENSION = 16;
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
    , cfg_work_(std::make_unique<executor_work_guard<io_context::executor_type>>(make_work_guard(cfg_executor_)))
    //    , thread_pool_(std::make_unique<thread_pool_t>(std::max(std::thread::hardware_concurrency(), 3u) - 2u))
    , thread_pool_(std::make_unique<thread_pool_t>(4))
    , window_system_(std::make_unique<gpu::window_system_s>())
    , gpu_(std::make_unique<gpu::device_s>(gpu_options(command_line_options_)))
    , decklink_registry_(nodes::decklink::decklink_registry_s::create_decklink_registry())
    , ndi_registry_(nodes::ndi::ndi_registry_s::create_ndi_registry())
    , font_registry_(render::font_registry_s::create_font_registry())
    , status_registry_(std::make_unique<node_status_registry_s>())
{
    fallback_texture_ = std::make_unique<gpu::texture_s>(
        *gpu_, gpu::vec2i_t{FALLBACK_TEXTURE_DIMENSION, FALLBACK_TEXTURE_DIMENSION}, gpu::format_e::rgba_unorm16);
    fallback_texture_->clear(commands());
    submit_gpu();
    texture_upload_service_   = std::make_unique<gpu::transfer::texture_upload_service_s>(*gpu_);
    texture_readback_service_ = std::make_unique<gpu::transfer::texture_readback_service_s>(*gpu_);
#if MIXIMUS_ENABLE_CEF
    try {
        auto profile = command_line_options_.settings_path;
        profile += ".cef";
        cef_subsystem_ = std::make_shared<nodes::cef::subsystem_s>(*gpu_, profile);
        cef_error_.clear();
    } catch (const std::exception& failure) {
        cef_error_ = failure.what();
        getlog("app")->error("CEF browser subsystem unavailable: {}", cef_error_);
    }
#endif
    cfg_thread_ = std::thread([this] { cfg_executor_.run(); });
}

app_state_s::app_state_s(test_state_t /*test_state*/, command_line_options_s command_line_options)
    : command_line_options_(std::move(command_line_options))
{
}

app_state_s::~app_state_s()
{
    if (std::uncaught_exceptions() != 0) {
        utils::start_shutdown_watchdog();
    }
    if (!window_system_) {
        return;
    }

    decklink_registry_->uninstall();
    cfg_work_ = nullptr;

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
    utils::begin_shutdown_step("GPU subsystem");
    abort_gpu();
    texture_readback_service_.reset();
    texture_upload_service_.reset();
    fallback_texture_.reset();
    last_submission_ = {};
    gpu_.reset();
    window_system_.reset();
    utils::report_shutdown_step_completed();
    utils::begin_shutdown_step("application worker services");
    cfg_executor_.stop();
    cfg_thread_.join();
    thread_pool_->close_queue();
    thread_pool_.reset();
}

} // namespace miximus::core
