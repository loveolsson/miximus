#pragma once

#include "gpu/device.hpp"
#include "gpu/texture_frame.hpp"
#include "transfer_layout.hpp"

#include <memory>
#include <string_view>

namespace miximus::gpu::transfer::detail {

enum class transfer_direction_e
{
    gpu_to_cpu,
    cpu_to_gpu,
};

// Private extension point for Vulkan, CUDA, and future registered-memory backends.
// The backend owns its host allocation and all registrations/synchronization. Its
// stable address is handed directly to SDK DMA through leases, never a CPU bounce
// buffer. Preserve the negotiated format/stride/size; stronger alignment is allowed.
// The frame must outlive the backend, including construction-failure cleanup.
class transfer_backend_i
{
  protected:
    transfer_backend_i() = default;

  public:
    transfer_backend_i(const transfer_backend_i&)            = delete;
    transfer_backend_i& operator=(const transfer_backend_i&) = delete;
    transfer_backend_i(transfer_backend_i&&)                 = delete;
    transfer_backend_i& operator=(transfer_backend_i&&)      = delete;
    virtual ~transfer_backend_i()                            = default;

    virtual void*  host_memory() const noexcept            = 0;
    virtual size_t host_buffer_size_bytes() const noexcept = 0;
    // Include host storage, native allocation padding and backend-private storage;
    // the frame's payload storage is accounted for separately by the transfer service.
    virtual size_t           allocation_bytes() const      = 0;
    virtual std::string_view backend_name() const noexcept = 0;

    // Called when granting a free upload lease, possibly on an SDK callback.
    // Must not wait, register memory, submit GPU work, or change the host address.
    virtual void prepare_host_write() = 0;
    // Worker-only: release/acquire GPU ownership and submit the transfer. A busy
    // recorder returns false for retry; failed operations throw, never report ready.
    virtual bool submit_transfer(const completion_s& dependency = {}) = 0;
    // Ticket for the complete Vulkan ownership hand-off, available after submission.
    virtual completion_s completion() const = 0;
    // Worker-only, nonblocking. True means DMA and ownership hand-offs completed,
    // and host readback visibility is established. A GPU-side wait alone is not
    // sufficient to release a producer lease or publish SDK-readable host memory.
    virtual bool transfer_ready() = 0;
    // Destruction runs on the service worker after external leases and frame uses
    // retire. Drain outstanding work, unregister resources, then unpin/free memory.
};

std::unique_ptr<transfer_backend_i> create_transfer_backend(device_s&                      device,
                                                            const texture_transfer_plan_s& plan,
                                                            transfer_direction_e           direction,
                                                            texture_frame_s&               frame,
                                                            recording_context_s*           recording_context);
} // namespace miximus::gpu::transfer::detail
