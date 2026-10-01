#pragma once

#include "external_image.hpp"
#include "gpu/recording.hpp"

namespace miximus::gpu::detail {

// Native external-video producer. Allocate off the render thread. The owner
// must hold one bounded lease from recording through actual consumer completion.
// Handles are borrowed; duplicate/transfer them through native IPC, never JSON.
class external_image_export_s
{
    struct state_s;
    std::shared_ptr<state_s> state_;

  public:
    external_image_export_s(device_s& device, extent_s extent);

    ~external_image_export_s();
    external_image_export_s(const external_image_export_s&)            = delete;
    external_image_export_s& operator=(const external_image_export_s&) = delete;

    external_image_s descriptor() const;
    size_t           allocation_bytes() const;

    // Record conversion and release to external ownership in GENERAL layout. Publish the
    // descriptor only after successful submission AND actual producer completion.
    // Before another copy, establish actual external consumer GPU completion and
    // release in GENERAL layout. This method cannot establish that from a handle.
    // Abandon the whole recording on failure. No concurrent uses of this exporter.
    void copy(recording_s& recording, const texture_s& source, const draw_s& conversion);
};

} // namespace miximus::gpu::detail
