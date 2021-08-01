#include "core/app_state.hpp"
#include "gpu/context.hpp"
#include "nodes/decklink/registry.hpp"

using namespace boost::asio;

namespace miximus::core {

app_state_s::app_state_s()
    : ctx_(std::make_unique<gpu::context_s>(false))
    , decklink_registry_(std::make_unique<nodes::decklink::decklink_registry_s>())
    , cfg_work_(std::make_unique<io_service::work>(cfg_executor_))
    , cfg_thread_(std::make_unique<std::thread>([this]() { cfg_executor_.run(); }))
{
}

app_state_s::~app_state_s()
{
    ctx_.reset();

    cfg_work_.reset();
    cfg_executor_.stop();

    if (cfg_thread_) {
        cfg_thread_->join();
    }
}

} // namespace miximus::core
