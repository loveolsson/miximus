#pragma once
#include <boost/asio/io_service.hpp>

#include <memory>
#include <thread>

namespace miximus::gpu {
class context;
class shader_store_s;
} // namespace miximus::gpu

namespace miximus::nodes::decklink {
class decklink_registry_s;
}

namespace miximus::core {

class app_state_s
{
    using io_service_t = boost::asio::io_service;

    std::unique_ptr<gpu::context>                         gpu_ctx_;
    std::unique_ptr<gpu::shader_store_s>                  shader_store_;
    std::unique_ptr<nodes::decklink::decklink_registry_s> decklink_registry_;

    io_service_t                        config_executor_;
    std::unique_ptr<io_service_t::work> config_work_;
    std::unique_ptr<std::thread>        config_thread_;

  public:
    app_state_s();
    ~app_state_s();

    io_service_t& get_config_executor() { return config_executor_; }
};

} // namespace miximus::core
