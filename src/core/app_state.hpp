#pragma once
#include "core/frame_context.hpp"
#include "core/node_status_registry_fwd.hpp"
#include "gpu/context_fwd.hpp"
#include "gpu/texture_fwd.hpp"
#include "gpu/transfer/texture_download_fwd.hpp"
#include "gpu/transfer/texture_upload_fwd.hpp"
#include "nodes/decklink/registry_fwd.hpp"
#include "nodes/frame_execution_fwd.hpp"
#include "nodes/ndi/registry_fwd.hpp"
#include "render/font/font_registry_fwd.hpp"
#include "types/frame_rate.hpp"
#include "utils/asio.hpp"

#include <FiberPool.hpp>
#include <chrono>
#include <memory>
#include <optional>
#include <thread>

namespace miximus::core {

class app_state_s
{
  public:
    struct frame_settings_s
    {
        struct decklink_output_settings_s
        {
            static constexpr int DEFAULT_PREROLL_FRAMES = 4;
            static constexpr int DEFAULT_BUFFER_FRAMES  = 4;
            static constexpr int MIN_BUFFER_FRAMES      = 1;
            static constexpr int MAX_BUFFER_FRAMES      = 8;

            int preroll_frames{DEFAULT_PREROLL_FRAMES};
            int buffer_frames{DEFAULT_BUFFER_FRAMES};
        };

        struct ndi_output_settings_s
        {
            static constexpr int DEFAULT_BUFFER_FRAMES = 4;
            static constexpr int MIN_BUFFER_FRAMES     = 1;
            static constexpr int MAX_BUFFER_FRAMES     = 8;

            int buffer_frames{DEFAULT_BUFFER_FRAMES};
        };

        frame_rate_s               frame_rate{DEFAULT_FRAME_RATE};
        decklink_output_settings_s decklink_output;
        ndi_output_settings_s      ndi_output;
    };

  private:
    using io_service_t  = boost::asio::io_context;
    using work_guard_t  = boost::asio::executor_work_guard<io_service_t::executor_type>;
    using thread_pool_t = FiberPool::FiberPool<true>;

    io_service_t                   cfg_executor_;
    std::unique_ptr<work_guard_t>  cfg_work_;
    std::thread                    cfg_thread_;
    std::unique_ptr<thread_pool_t> thread_pool_;

    std::unique_ptr<gpu::context_s>                            ctx_;
    std::unique_ptr<gpu::texture_s>                            fallback_texture_;
    std::unique_ptr<gpu::transfer::texture_upload_service_s>   texture_upload_service_;
    std::unique_ptr<gpu::transfer::texture_download_service_s> texture_download_service_;
    std::unique_ptr<nodes::decklink::decklink_registry_s>      decklink_registry_;
    std::unique_ptr<nodes::ndi::ndi_registry_s>                ndi_registry_;
    std::unique_ptr<render::font_registry_s>                   font_registry_;
    std::unique_ptr<node_status_registry_s>                    status_registry_;

    frame_settings_s frame_settings_{};
    frame_context_s  frame_context_{};

  public:
    // Builds only frame-local state so graph lifecycle tests do not initialize
    // hardware, worker threads, or OpenGL resources.
    struct test_state_t
    {
        explicit test_state_t() = default;
    };

    app_state_s();
    explicit app_state_s(test_state_t);
    ~app_state_s();

    auto cfg_executor() { return &cfg_executor_; }
    auto ctx() { return ctx_.get(); }
    auto fallback_texture() { return fallback_texture_.get(); }
    auto texture_upload_service() { return texture_upload_service_.get(); }
    auto texture_download_service() { return texture_download_service_.get(); }
    auto decklink_registry() { return decklink_registry_.get(); }
    auto ndi_registry() { return ndi_registry_.get(); }
    auto font_registry() { return font_registry_.get(); }
    auto thread_pool() { return thread_pool_.get(); }
    auto status_registry() { return status_registry_.get(); }

    void begin_frame(frame_settings_s settings, frame_context_s frame_context);

    const frame_settings_s& frame_settings() const { return frame_settings_; }
    const frame_context_s&  frame_context() const { return frame_context_; }

    struct
    {
        nodes::submitted_node_set_t submitted_nodes;
        nodes::executed_node_set_t  executed_nodes;
    } frame_info;
};

} // namespace miximus::core
