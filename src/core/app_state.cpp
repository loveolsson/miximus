#include "app_state.hpp"

#include "core/application_settings.hpp"
#include "core/node_status_registry.hpp"
#include "gpu/context.hpp"
#include "gpu/texture.hpp"
#include "gpu/transfer/detail/backend_factory.hpp"
#include "gpu/transfer/texture_download.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "nodes/decklink/registry.hpp"
#include "nodes/ndi/registry.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"

#include <memory>
#include <utility>

using namespace boost::asio;

namespace miximus::core {
namespace {
constexpr int FALLBACK_TEXTURE_DIMENSION = 16;
}

void app_state_s::begin_frame(const application_settings_snapshot_s& settings, frame_context_s frame_context)
{
    frame_rate_           = settings.frame_rate;
    frame_context_        = frame_context;
    frame_info.timestamp  = frame_context.target_time;
    frame_info.pts        = frame_context.pts;
    frame_info.duration   = frame_context.duration;
    frame_info.field_even = frame_context.frame_number % 2 == 0;
}

app_state_s::app_state_s()
    : cfg_work_(std::make_unique<executor_work_guard<io_context::executor_type>>(make_work_guard(cfg_executor_)))
    , cfg_thread_([this] { cfg_executor_.run(); })
    //    , thread_pool_(std::make_unique<thread_pool_t>(std::max(std::thread::hardware_concurrency(), 3u) - 2u))
    , thread_pool_(std::make_unique<thread_pool_t>(4))
    , ctx_(gpu::context_s::create_unique_context(false, nullptr))
    , decklink_registry_(nodes::decklink::decklink_registry_s::create_decklink_registry())
    , ndi_registry_(nodes::ndi::ndi_registry_s::create_ndi_registry())
    , font_registry_(render::font_registry_s::create_font_registry())
    , status_registry_(std::make_unique<node_status_registry_s>())
{
    frame_info.duration = *get_frame_duration(DEFAULT_FRAME_RATE);

    // Transfer backend initialization must happen on the root GL context. It is
    // intentionally part of app startup rather than context construction so a
    // failed optional backend simply selects the persistent-PBO implementation.
    const gpu::context_scope_s context_scope(*ctx_);
    auto                       fallback_texture = std::make_unique<gpu::texture_s>(
        gpu::vec2i_t{FALLBACK_TEXTURE_DIMENSION, FALLBACK_TEXTURE_DIMENSION}, gpu::texture_s::format_e::rgba_f16);
    fallback_texture->clear();
    gpu::transfer::detail::initialize_backends();
    texture_upload_service_   = std::make_unique<gpu::transfer::texture_upload_service_s>(ctx_.get());
    texture_download_service_ = std::make_unique<gpu::transfer::texture_download_service_s>(ctx_.get());
    fallback_texture_         = std::move(fallback_texture);
}

app_state_s::~app_state_s()
{
    decklink_registry_->uninstall();
    cfg_work_ = nullptr;

    // Capture-control work may still be retiring SDK buffers and upload
    // streams after a node was removed. Drain it before destroying the shared
    // transfer services it uses.
    decklink_registry_.reset();

    // DVP is tied to the root GL context and must be closed before that context
    // is destroyed. Nodes and their transfers are destroyed before app_state.
    texture_download_service_.reset();
    texture_upload_service_.reset();
    {
        const gpu::context_scope_s context_scope(*ctx_);
        fallback_texture_.reset();
        gpu::transfer::detail::shutdown_backends();
    }
    ctx_.reset();
    cfg_executor_.stop();
    cfg_thread_.join();
    thread_pool_->close_queue();
}

} // namespace miximus::core
