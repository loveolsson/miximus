#pragma once
#include "gpu/framebuffer_fwd.hpp"
#include "gpu/types.hpp"
#include "utils/flicks.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace miximus::gpu {
class context_s;
}

namespace miximus::nodes::screen::detail {

struct output_presenter_metrics_s
{
    uint64_t frames_submitted{};
    uint64_t program_queue_overflow_drops{};
    uint64_t program_timing_drops{};
    uint64_t program_frames_repeated{};
    uint64_t program_frames_missing{};
    uint64_t output_intervals_skipped{};
    uint64_t swaps_completed{};
    uint64_t render_acquire_misses{};
    size_t   queued_frames{};
    size_t   slots{};
    size_t   free_slots{};
    size_t   retiring_slots{};
    int64_t  output_latency_us{};
    int64_t  program_selection_offset_us{};
    int64_t  completion_interval_max_us{};
    double   measured_refresh_hz{};
    bool     uses_nominal_cadence{};
};

class output_presenter_s
{
    class impl_s;

  public:
    class render_frame_s
    {
        impl_s* impl_{};
        size_t  slot_index_{};

        render_frame_s(impl_s* impl, size_t slot_index) noexcept;
        friend class output_presenter_s;

      public:
        render_frame_s() = default;
        ~render_frame_s();

        render_frame_s(const render_frame_s&)            = delete;
        render_frame_s& operator=(const render_frame_s&) = delete;
        render_frame_s(render_frame_s&& other) noexcept;
        render_frame_s& operator=(render_frame_s&& other) noexcept;

        gpu::framebuffer_s* target() const noexcept;
        void                submit(utils::flicks target_time);
    };

  private:
    std::unique_ptr<impl_s> impl_;

  public:
    output_presenter_s(gpu::context_s* root_context, size_t buffer_frames, utils::flicks nominal_frame_duration);
    ~output_presenter_s();

    output_presenter_s(const output_presenter_s&)            = delete;
    output_presenter_s& operator=(const output_presenter_s&) = delete;
    output_presenter_s(output_presenter_s&&)                 = delete;
    output_presenter_s& operator=(output_presenter_s&&)      = delete;

    gpu::context_s* context() noexcept;
    void            start();
    void            request_stop() noexcept;
    bool            stopped() const noexcept;
    void            stop();

    std::optional<render_frame_s> try_acquire(gpu::vec2i_t dimensions);
    output_presenter_metrics_s    metrics() const;
};

} // namespace miximus::nodes::screen::detail
