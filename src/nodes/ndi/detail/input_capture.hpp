#pragma once
#include "gpu/texture_fwd.hpp"
#include "gpu/transfer/texture_upload_fwd.hpp"
#include "gpu/types.hpp"
#include "media/timed_source_queue.hpp"
#include "utils/flicks.hpp"
#include "utils/serial_executor_fwd.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace miximus::nodes::ndi::detail {

struct resolved_input_frame_s
{
    gpu::texture_s* texture{};
    gpu::vec2i_t    dimensions{};
};

class input_capture_s : public std::enable_shared_from_this<input_capture_s>
{
  public:
    enum class phase_e : uint8_t
    {
        starting,
        running,
        stopping,
        stopped,
        failed,
    };

    struct metrics_s
    {
        uint64_t                            frames_received{};
        uint64_t                            invalid_frames{};
        uint64_t                            receiver_video_drops{};
        uint32_t                            receiver_queue_depth{};
        uint64_t                            upload_slot_drops{};
        media::timed_source_queue_metrics_s source_queue;
    };

  private:
    class impl_s;
    std::unique_ptr<impl_s> impl_;

    input_capture_s(gpu::transfer::texture_upload_service_s* upload_service,
                    utils::serial_executor_s*                control_executor,
                    std::string                              source_name,
                    std::string                              receiver_name);

  public:
    static std::shared_ptr<input_capture_s> create(gpu::transfer::texture_upload_service_s* upload_service,
                                                   utils::serial_executor_s*                control_executor,
                                                   std::string                              source_name,
                                                   std::string                              receiver_name);

    ~input_capture_s();

    input_capture_s(const input_capture_s&)            = delete;
    input_capture_s& operator=(const input_capture_s&) = delete;
    input_capture_s(input_capture_s&&)                 = delete;
    input_capture_s& operator=(input_capture_s&&)      = delete;

    void start_async();
    void stop_async();

    phase_e   phase() const;
    metrics_s metrics() const;

    void advance_frames(utils::flicks program_pts, utils::flicks target_time, bool discontinuity);
    bool submit_frame(utils::flicks program_pts);
    std::optional<resolved_input_frame_s> resolve_frame();
    void                                  release_prepared_frame();
    void                                  reset_frames();
};

} // namespace miximus::nodes::ndi::detail
