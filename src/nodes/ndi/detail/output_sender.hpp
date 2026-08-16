#pragma once
#include "gpu/transfer/texture_readback_fwd.hpp"
#include "gpu/types.hpp"
#include "types/frame_rate.hpp"
#include "utils/flicks.hpp"
#include "utils/serial_executor_fwd.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace miximus::nodes::ndi::detail {

class output_sender_s : public std::enable_shared_from_this<output_sender_s>
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
        uint64_t program_frames_received{};
        uint64_t program_queue_overflow_drops{};
        uint64_t program_timing_drops{};
        uint64_t program_frames_repeated{};
        uint64_t program_frames_missing{};
        uint64_t output_intervals_skipped{};
        uint64_t frames_sent{};
        size_t   queued_frames{};
        int64_t  output_latency_us{};
        int64_t  program_selection_offset_us{};
    };

  private:
    static constexpr size_t RETAINED_PROGRAM_FRAME_COUNT = 1;
    static constexpr size_t READBACK_PIPELINE_HEADROOM   = 2;

    static size_t validate_buffer_frame_count(size_t buffer_frames);

    class impl_s;
    std::unique_ptr<impl_s> impl_;

    output_sender_s(utils::serial_executor_s* control_executor, std::string sender_name);

  public:
    static size_t get_queue_capacity(size_t buffer_frames);
    static size_t get_readback_slot_count(size_t buffer_frames);

    static std::shared_ptr<output_sender_s> create(utils::serial_executor_s* control_executor, std::string sender_name);

    ~output_sender_s();

    output_sender_s(const output_sender_s&)            = delete;
    output_sender_s& operator=(const output_sender_s&) = delete;
    output_sender_s(output_sender_s&&)                 = delete;
    output_sender_s& operator=(output_sender_s&&)      = delete;

    void start_async();
    void stop_async();

    phase_e   phase() const;
    metrics_s metrics() const;

    void set_stream(std::shared_ptr<gpu::transfer::texture_readback_stream_s> stream,
                    gpu::vec2i_t                                              dimensions,
                    frame_rate_s                                              frame_rate,
                    utils::flicks                                             frame_duration,
                    utils::flicks                                             program_time_origin,
                    size_t                                                    buffer_frames);
    void clear_stream();
    void notify_frame();
};

} // namespace miximus::nodes::ndi::detail
