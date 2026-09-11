#pragma once

#include "device.hpp"
#include "utils/flicks.hpp"

#include <functional>
#include <optional>

struct GLFWwindow;

namespace miximus::gpu {

struct presentation_metrics_s
{
    uint64_t    presents{};
    uint64_t    acquire_misses{};
    uint64_t    mailbox_drops{};
    uint64_t    recreations{};
    uint64_t    present_call_max_us{};
    std::string failure;
    bool        stopped{};
};

struct presentation_frame_s
{
    texture_s                   image;
    completion_s                ready;
    std::shared_ptr<const void> lease;
};

enum class presentation_pacing_e : uint8_t
{
    display,
    application,
};

enum class presentation_timing_e : uint8_t
{
    present_wait,
    submission_estimate,
};

struct presentation_feedback_s
{
    utils::flicks         time;
    presentation_timing_e timing;
};

struct presentation_source_s
{
    // Called after acquisition, outside GPU recording. Select by timestamp;
    // the GPU waits for the returned frame's dependency before copying it.
    std::function<std::optional<presentation_frame_s>(presentation_pacing_e, const std::stop_token&)> next_frame;
    // Present-wait feedback follows display completion, with host wake-up error.
    // Devices without that extension explicitly report a submission estimate.
    std::function<void(presentation_feedback_s)> complete;
};

// Specialized WSI boundary. Window creation/events/size queries belong to the
// main thread. The worker owns acquisition/presentation and bounded WSI state.
class presenter_s
{
    std::unique_ptr<detail::presenter_state_s> state_;

  public:
    presenter_s(device_s& device, GLFWwindow* window, extent_s initial_extent, presentation_source_s source = {});
    ~presenter_s();
    presenter_s(const presenter_s&)            = delete;
    presenter_s& operator=(const presenter_s&) = delete;
    // Schedules one presentation (latest wins while busy). Repeats are explicit.
    // Retains the image for resize; the producer must not overwrite it while retained.
    void                   publish(texture_s image, completion_s ready, std::shared_ptr<const void> lease = {});
    void                   resize(extent_s extent);
    void                   request_stop() noexcept;
    presentation_metrics_s metrics() const;
};
} // namespace miximus::gpu
