#pragma once
#include "device_reservation.hpp"
#include "gpu/texture_fwd.hpp"
#include "gpu/transfer/texture_upload_fwd.hpp"
#include "gpu/types.hpp"
#include "media/timed_source_queue.hpp"
#include "utils/flicks.hpp"
#include "utils/serial_executor.hpp"
#include "wrapper/decklink-sdk/decklink_inc.hpp"
#include "wrapper/decklink-sdk/decklink_ptr.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace miximus::nodes::decklink::detail {

struct captured_input_frame_s
{
    gpu::texture_s* texture{};
    gpu::vec2i_t    dimensions{};
    BMDColorspace   colorspace{bmdColorspaceRec709};
};

class input_capture_s
{
  public:
    enum class phase_e : uint8_t
    {
        starting,
        running,
        release_requested,
        reconfiguring,
        stopping,
        stopped,
        failed,
    };

    struct metrics_s
    {
        uint64_t                            frames_received{};
        uint64_t                            frames_missing{};
        uint64_t                            no_input_source_frames{};
        uint64_t                            upload_slot_drops{};
        uint32_t                            available_video_frames{};
        media::timed_source_queue_metrics_s source_queue;
    };

  private:
    class impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    input_capture_s(gpu::transfer::texture_upload_service_s*              upload_service,
                    utils::serial_executor_s*                             control_executor,
                    decklink_sdk::decklink_ptr<IDeckLinkInput>            device,
                    std::shared_ptr<device_reservation_s<IDeckLinkInput>> reservation,
                    std::string                                           device_name);
    ~input_capture_s();

    input_capture_s(const input_capture_s&)            = delete;
    input_capture_s(input_capture_s&&)                 = delete;
    input_capture_s& operator=(const input_capture_s&) = delete;
    input_capture_s& operator=(input_capture_s&&)      = delete;

    void start_async();
    void stop_async();
    void acknowledge_render_release();

    void advance_frames(utils::flicks program_pts, utils::flicks target_time, bool discontinuity);
    bool submit_frame(utils::flicks program_pts);
    std::optional<captured_input_frame_s> resolve_frame();
    void                                  release_prepared_frame();
    void                                  reset_frames();

    bool      requires_render_release() const;
    phase_e   phase() const;
    metrics_s metrics() const;
};

} // namespace miximus::nodes::decklink::detail
