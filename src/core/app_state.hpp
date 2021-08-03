#pragma once
#include "utils/asio.hpp"

#include "gpu/context_fwd.hpp"
#include "nodes/decklink/registry_fwd.hpp"
#include "render/font/registry_fwd.hpp"

#include <memory>
#include <thread>

namespace miximus::core {

class app_state_s
{
    using io_service_t = boost::asio::io_service;

    io_service_t                        cfg_executor_;
    std::unique_ptr<io_service_t::work> cfg_work_;
    std::unique_ptr<std::thread>        cfg_thread_;

    std::unique_ptr<gpu::context_s>                       ctx_;
    std::unique_ptr<nodes::decklink::decklink_registry_s> decklink_registry_;
    std::unique_ptr<render::font::font_registry_s>        font_registry_;

  public:
    app_state_s();
    ~app_state_s();

    auto* cfg_executor() { return &cfg_executor_; }
    auto* ctx() { return ctx_.get(); }
    auto* decklink_registry() { return decklink_registry_.get(); }
    auto* font_registry() { return font_registry_.get(); }
};

} // namespace miximus::core
