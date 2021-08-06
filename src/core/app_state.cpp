#include "app_state.hpp"
#include "gpu/context.hpp"
#include "nodes/decklink/registry.hpp"
#include "render/font/font_loader.hpp"
#include "render/font/font_registry.hpp"
#include "utils/bind.hpp"

using namespace boost::asio;

namespace miximus::core {

app_state_s::app_state_s()
    : font_registry_(render::font_registry_s::create_font_registry())
    , cfg_work_(std::make_unique<io_service::work>(cfg_executor_))
    , cfg_thread_([this] { cfg_executor_.run(); })
    , thread_pool_(std::max(std::thread::hardware_concurrency(), 3u) - 2u)
    , ctx_(gpu::context_s::create_unique_context(false, nullptr))
    , decklink_registry_(nodes::decklink::decklink_registry_s::create_decklink_registry())
{
}

app_state_s::~app_state_s()
{
    decklink_registry_->uninstall();

    cfg_work_.reset();
    ctx_.reset();

    cfg_executor_.stop();
    cfg_thread_.join();

    thread_pool_.close_queue();
}

} // namespace miximus::core
