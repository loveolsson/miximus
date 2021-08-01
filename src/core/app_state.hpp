#pragma once
#include "utils/asio.hpp"

#include <memory>
#include <thread>

namespace miximus::gpu {
class context_s;
} // namespace miximus::gpu

namespace miximus::nodes::decklink {
class decklink_registry_s;
}

namespace miximus::core {

class app_state_s
{
    using io_service_t = boost::asio::io_service;

    std::unique_ptr<gpu::context_s>                       ctx_;
    std::unique_ptr<nodes::decklink::decklink_registry_s> decklink_registry_;

    io_service_t                        cfg_executor_;
    std::unique_ptr<io_service_t::work> cfg_work_;
    std::unique_ptr<std::thread>        cfg_thread_;

  public:
    app_state_s();
    ~app_state_s();

    auto& cfg_executor() { return cfg_executor_; }
    auto& ctx() { return *ctx_; }
    auto& decklink_registry() { return *decklink_registry_; }
};

} // namespace miximus::core
