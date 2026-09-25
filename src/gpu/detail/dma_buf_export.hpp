#pragma once

#include "dma_buf_image.hpp"
#include "gpu/recording.hpp"

namespace miximus::gpu::detail {

// Linux-only external-video producer. Allocate off the render thread. The owner
// must hold one bounded lease from recording through actual consumer completion.
// Handles are borrowed; duplicate/transfer them through native IPC, never JSON.
class dma_buf_export_s
{
    struct state_s;
    std::shared_ptr<state_s> state_;

  public:
    dma_buf_export_s(device_s& device, extent_s extent);

    ~dma_buf_export_s();
    dma_buf_export_s(const dma_buf_export_s&)            = delete;
    dma_buf_export_s& operator=(const dma_buf_export_s&) = delete;

    dma_buf_image_s descriptor() const;
    size_t          allocation_bytes() const;

    // Record conversion and release to FOREIGN in GENERAL layout. Publish the
    // descriptor only after successful submission AND actual producer completion.
    // Before another copy, establish actual external consumer GPU completion and
    // release in GENERAL layout. This method cannot establish that from an FD.
    // Abandon the whole recording on failure. No concurrent uses of this exporter.
    void copy(recording_s& recording, const texture_s& source, const draw_s& conversion);
};

} // namespace miximus::gpu::detail
