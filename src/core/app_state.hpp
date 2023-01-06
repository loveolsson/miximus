#pragma once
#include "utils/asio.hpp"

#include "gpu/context_fwd.hpp"
#include "nodes/decklink/registry_fwd.hpp"
#include "render/font/font_registry_fwd.hpp"
#include "utils/flicks.hpp"

#include <optional>

#include <FiberPool.hpp>

#include <chrono>
#include <memory>
#include <thread>

namespace miximus::core {

class app_state_s
{
    using io_service_t  = boost::asio::io_service;
    using thread_pool_t = FiberPool::FiberPool<true>;

    io_service_t                        cfg_executor_;
    std::unique_ptr<io_service_t::work> cfg_work_;
    std::thread                         cfg_thread_;
    std::unique_ptr<thread_pool_t>      thread_pool_;

    std::unique_ptr<gpu::context_s>                       ctx_;
    std::unique_ptr<nodes::decklink::decklink_registry_s> decklink_registry_;
    std::unique_ptr<render::font_registry_s>              font_registry_;

  public:
    app_state_s();
    ~app_state_s();

    auto* cfg_executor() { return &cfg_executor_; }
    auto* ctx() { return ctx_.get(); }
    auto* decklink_registry() { return decklink_registry_.get(); }
    auto* font_registry() { return font_registry_.get(); }
    auto* thread_pool() { return thread_pool_.get(); }

    struct
    {
        utils::flicks timestamp;
        utils::flicks pts;
        utils::flicks duration;
        bool          field_even{};
    } frame_info;
};

} // namespace miximus::core
