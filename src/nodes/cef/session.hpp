#pragma once

#include "core/frame_context.hpp"
#include "detail/capture_timing.hpp"
#include "detail/frame_pool.hpp"
#include "media/timed_source_queue.hpp"
#include "media_input_types.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace miximus::nodes::cef {

// One browser and one immutable viewport generation. Construction allocates
// GPU resources and belongs on the subsystem's control worker, never prepare().
// CEF callbacks produce completed owned frames; only the render thread consumes
// the timed queue. The subsystem must close/drain sessions before CEF shutdown.
class session_s
{
  public:
    struct options_s
    {
        std::string  url;
        gpu::vec2i_t dimensions{1920, 1080};
        int          frame_rate{60};
        bool         operator==(const options_s&) const = default;
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
        phase_e                              phase{};
        std::string                          error;
        uint64_t                             received{};
        uint64_t                             copied{};
        uint64_t                             dropped{};
        uint64_t                             timing_rejections{};
        std::string                          timing_error;
        detail::capture_timing_s::snapshot_s capture;
        detail::capture_timing_s::snapshot_s completion_wait;
        media::timed_source_queue_metrics_s  source_queue;
        media_input_metrics_s                inputs;
    };

    using frame_ptr_t = std::shared_ptr<const detail::frame_pool_s::frame_s>;

    struct command_result_s
    {
        // A serialized JSON value on success; empty on failure.
        std::string json;
        std::string error;
    };

  protected:
    struct impl_s;
    std::unique_ptr<impl_s> impl_;
    session_s(gpu::device_s& device, options_s options, std::shared_ptr<detail::media_input_runtime_s> inputs = {});
    void reset_frames();

  public:
    ~session_s();
    session_s(const session_s&)            = delete;
    session_s& operator=(const session_s&) = delete;

    // Internal trusted-native capability, not a public page/control protocol.
    // The function receives parsed JSON and may return a value or Promise.
    // Futures must never be waited on from prepare/execute/complete.
    std::future<command_result_s>
    request(std::string function_source, std::string json, std::chrono::milliseconds timeout = std::chrono::seconds(5));
    bool context_ready() const noexcept;
    // Cooperative timing is opt-in through this internal native capability.
    // Navigation invalidates the handler. No rAF override or paint correlation.
    std::future<command_result_s> set_program_time_handler(std::string function_source);
    void                          send_program_time(core::frame_context_s time);

    // Render-thread methods, matching the existing media input lifecycle.
    void                  advance_frames(utils::flicks pts, utils::flicks target_time, bool discontinuity);
    bool                  submit_frame(utils::flicks pts);
    frame_ptr_t           resolve_frame();
    void                  release_prepared_frame();
    metrics_s             metrics() const;
    uint32_t              media_input_demand() const;
    std::function<void()> record_media_input(size_t                input,
                                             gpu::recording_s&     commands,
                                             const gpu::texture_s* source,
                                             std::string_view      source_node,
                                             std::string_view      source_interface,
                                             int64_t               timestamp_us);
};

} // namespace miximus::nodes::cef
