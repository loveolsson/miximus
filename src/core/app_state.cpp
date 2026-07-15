#include "app_state.hpp"

#include "core/node_status_registry.hpp"
#include "gpu/context.hpp"
#include "gpu/transfer/detail/backend_factory.hpp"
#include "gpu/transfer/texture_download.hpp"
#include "gpu/transfer/texture_upload.hpp"
#include "nodes/decklink/registry.hpp"
#include "nodes/ndi/registry.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"

#include <memory>

using namespace boost::asio;

namespace miximus::core {

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
    // Transfer backend initialization must happen on the root GL context. It is
    // intentionally part of app startup rather than context construction so a
    // failed optional backend simply selects the persistent-PBO implementation.
    const gpu::context_scope_s context_scope(*ctx_);
    gpu::transfer::detail::initialize_backends();
    texture_upload_service_   = std::make_unique<gpu::transfer::texture_upload_service_s>(ctx_.get());
    texture_download_service_ = std::make_unique<gpu::transfer::texture_download_service_s>(ctx_.get());
}

app_state_s::~app_state_s()
{
    decklink_registry_->uninstall();
    cfg_work_ = nullptr;

    // DVP is tied to the root GL context and must be closed before that context
    // is destroyed. Nodes and their transfers are destroyed before app_state.
    texture_download_service_.reset();
    texture_upload_service_.reset();
    {
        const gpu::context_scope_s context_scope(*ctx_);
        gpu::transfer::detail::shutdown_backends();
    }
    ctx_.reset();
    cfg_executor_.stop();
    cfg_thread_.join();
    thread_pool_->close_queue();
}

} // namespace miximus::core
