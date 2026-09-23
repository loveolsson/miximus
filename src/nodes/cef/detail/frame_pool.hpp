#pragma once

#include "gpu/device.hpp"
#include "gpu/texture.hpp"

#include <cstddef>
#include <memory>

namespace miximus::nodes::cef::detail {

// One immutable pool generation. Allocate off the frame/callback hot path and
// replace the whole pool on resize; outstanding frames retain the old generation.
class frame_pool_s
{
    struct state_s;
    std::shared_ptr<state_s> state_;

  public:
    class frame_s
    {
        std::shared_ptr<state_s> state_;
        size_t                   index_{};

        frame_s() = default;
        friend class frame_pool_s;

      public:
        ~frame_s();
        frame_s(const frame_s& other)            = delete;
        frame_s& operator=(const frame_s& other) = delete;

        // Producer writes through its own recording context, then publishes a
        // const lease. The lease alone does not mean the GPU write is complete.
        gpu::texture_s&       texture() noexcept;
        const gpu::texture_s& texture() const noexcept;
    };

    frame_pool_s(gpu::device_s& device, gpu::vec2i_t dimensions, size_t capacity, size_t memory_budget_bytes);
    frame_pool_s(const frame_pool_s& other)            = delete;
    frame_pool_s& operator=(const frame_pool_s& other) = delete;

    // Thread-safe, bounded, no GPU waits or texture allocation. Reuse requires
    // both the final lease release and retirement of all recorded/submitted uses.
    [[nodiscard]] std::shared_ptr<frame_s> try_acquire();
    // Retirement requires both released leases and completed GPU uses.
    bool idle() const;
};

} // namespace miximus::nodes::cef::detail
