#include "core/app_state.hpp"
#include "gpu/context.hpp"
#include "gpu/shader.hpp"
#include "nodes/decklink/decklink.hpp"

namespace miximus::core {

app_state_s::app_state_s()
{
    using namespace boost::asio;

    gpu_ctx_           = std::make_unique<gpu::context>();
    shader_store_      = std::make_unique<gpu::shader_store_s>();
    decklink_registry_ = std::make_unique<nodes::decklink::decklink_registry_s>();

    config_work_   = std::make_unique<io_service::work>(config_executor_);
    config_thread_ = std::make_unique<std::thread>([this]() { config_executor_.run(); });
}

app_state_s::~app_state_s()
{
    shader_store_.reset();
    gpu_ctx_.reset();
    gpu::context::terminate();

    config_work_.reset();
    config_executor_.stop();

    if (config_thread_) {
        config_thread_->join();
    }
}

} // namespace miximus::core
