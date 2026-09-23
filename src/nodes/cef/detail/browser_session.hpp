#pragma once

#include "frame_pool.hpp"
#include "media/timed_source_queue.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>

namespace miximus::nodes::cef::detail {

// One browser and one immutable viewport generation. Construction allocates
// GPU resources and belongs on the subsystem's control worker, never prepare().
// CEF callbacks produce completed owned frames; only the render thread consumes
// the timed queue. The subsystem must close/drain sessions before CEF shutdown.
class browser_session_s
{
  public:
    struct options_s
    {
        std::string  url;
        gpu::vec2i_t dimensions{1920, 1080};
        int          frame_rate{60};
        bool         transparent{true};
    };

    enum class phase_e : uint8_t
    {
        starting,
        loading,
        ready,
        closing,
        closed,
        failed
    };

    struct metrics_s
    {
        phase_e                             phase{};
        std::string                         error;
        uint64_t                            received{};
        uint64_t                            copied{};
        uint64_t                            dropped{};
        media::timed_source_queue_metrics_s source_queue;
    };

    using frame_ptr_t = std::shared_ptr<const frame_pool_s::frame_s>;

  private:
    struct impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    browser_session_s(gpu::device_s& device, options_s options);
    ~browser_session_s();
    browser_session_s(const browser_session_s&)            = delete;
    browser_session_s& operator=(const browser_session_s&) = delete;

    void start_async();
    void close_async();
    bool closed() const noexcept;
    // Control/shutdown worker only; never wait for browser closure on a frame.
    bool wait_closed(std::chrono::milliseconds timeout) const;

    // Render-thread methods, matching the existing media input lifecycle.
    void        advance_frames(utils::flicks pts, utils::flicks target_time, bool discontinuity);
    bool        submit_frame(utils::flicks pts);
    frame_ptr_t resolve_frame();
    void        release_prepared_frame();
    void        reset_frames();
    metrics_s   metrics() const;
};

} // namespace miximus::nodes::cef::detail
