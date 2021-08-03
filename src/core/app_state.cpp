#include "app_state.hpp"
#include "gpu/context.hpp"
#include "nodes/decklink/registry.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"

using namespace boost::asio;

namespace miximus::core {

app_state_s::app_state_s()
    : ctx_(gpu::context_s::create_unique_context(false, nullptr))
    , decklink_registry_(nodes::decklink::decklink_registry_s::create_decklink_registry())
    , font_registry_(render::font_registry_s::create_font_registry())
    , cfg_work_(std::make_unique<io_service::work>(cfg_executor_))
    , cfg_thread_(std::make_unique<std::thread>([this]() { cfg_executor_.run(); }))
{
}

app_state_s::~app_state_s()
{
    decklink_registry_->uninstall();

    ctx_.reset();

    cfg_work_.reset();
    cfg_executor_.stop();

    if (cfg_thread_) {
        cfg_thread_->join();
    }
}

} // namespace miximus::core
