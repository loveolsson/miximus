#pragma once

#include "resource_types.hpp"

#include <cstddef>
#include <memory>
#include <span>

namespace miximus::gpu {

struct allocation_info_s
{
    size_t bytes{};
    bool   device_local{};
    bool   host_coherent{};
    bool   host_cached{};
};

// Mapped spans borrow buffer storage. Callers must retain the buffer and stop
// accessing the span before GPU use. Fresh host access is rejected while a
// recording or submitted GPU use holds the buffer.
class buffer_s
{
    std::shared_ptr<detail::buffer_state_s> state_;

    explicit buffer_s(std::shared_ptr<detail::buffer_state_s> state);
    friend class device_s;
    // GPU-only hardware comparison probe; no production recording changes.
    friend class detail::color_comparison_s;
    friend class recording_s;
    friend class transfer::detail::cuda_transfer_s;

  public:
    buffer_s() = default;

    size_t                     size() const;
    bool                       idle() const;
    allocation_info_s          allocation_info() const;
    std::span<std::byte>       writable_bytes();
    std::span<const std::byte> readable_bytes() const;
    explicit                   operator bool() const noexcept { return state_ != nullptr; }
};

} // namespace miximus::gpu
