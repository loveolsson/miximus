#pragma once

#include "include/cef_render_handler.h"
#include "nodes/cef/session.hpp"

#include <atomic>
#include <mutex>

namespace miximus::nodes::cef::detail {

// UI-thread producer and render-thread consumer, with immutable pool generation.
class capture_stream_s
{
    using frame_ptr_t                       = session_s::frame_ptr_t;
    using frame_queue_t                     = media::timed_source_queue_s<frame_ptr_t>;
    static constexpr size_t  FRAME_CAPACITY = 8;
    static constexpr size_t  FRAME_BUDGET   = 1024ULL * 1024 * 1024;
    gpu::vec2i_t             dimensions_;
    int                      frame_rate_;
    frame_pool_s             pool_;
    gpu::recording_context_s context_;
    uint64_t                 epoch_{1};
    std::optional<uint64_t>  previous_timestamp_;
    std::atomic_uint64_t     received{}, copied{}, dropped{};
    mutable std::mutex       mutex;
    capture_timing_s         capture_timing, completion_wait_timing;
    frame_queue_t            frames{
                   {.capacity = 4, .playout_delay_frames = 1}
    };
    std::optional<frame_queue_t::ticket_t> prepared;

  public:
    capture_stream_s(gpu::device_s& device, gpu::vec2i_t dimensions, int frame_rate);
    static size_t texture_budget(gpu::vec2i_t dimensions);
    bool          capture(const CefAcceleratedPaintInfo& info, const std::atomic_bool& close_requested);
    void          new_epoch()
    {
        ++epoch_;
        previous_timestamp_.reset();
    }
    bool                 idle() const { return pool_.idle(); }
    void                 advance_frames(utils::flicks pts, utils::flicks target_time, bool discontinuity);
    bool                 submit_frame(utils::flicks pts);
    frame_ptr_t          resolve_frame();
    void                 release_prepared_frame();
    void                 reset_frames();
    session_s::metrics_s metrics() const;
};
} // namespace miximus::nodes::cef::detail
