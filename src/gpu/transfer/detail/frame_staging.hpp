#pragma once

#include "transfer_backend.hpp"

namespace miximus::gpu::transfer::detail {

// Slot-facing facade; services and SDK leases do not assume staging buffers or
// a particular allocator. A direct-memory backend can own the same contract.
class frame_staging_s
{
  public:
    using direction_e = transfer_direction_e;

  private:
    std::unique_ptr<transfer_backend_i> backend_;
    direction_e                         direction_;
    bool                                reported_completion_{};

  public:
    frame_staging_s(device_s&                      device,
                    const texture_transfer_plan_s& plan,
                    direction_e                    direction,
                    texture_frame_s*               frame,
                    recording_context_s*           recording_context = nullptr);
    size_t           host_buffer_size_bytes() const noexcept { return backend_->host_buffer_size_bytes(); }
    size_t           allocation_bytes() const { return backend_->allocation_bytes(); }
    std::string_view backend_name() const noexcept { return backend_->backend_name(); }
    void*            host_memory() const noexcept { return backend_->host_memory(); }
    void             prepare_host_write() { backend_->prepare_host_write(); }
    bool         submit_transfer(const completion_s& dependency = {}) { return backend_->submit_transfer(dependency); }
    completion_s completion() const { return backend_->completion(); }
    bool         transfer_ready();
};
} // namespace miximus::gpu::transfer::detail
