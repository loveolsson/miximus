#pragma once

#include "external_image.hpp"
#include "gpu/recording.hpp"

#include <chrono>

namespace miximus::gpu::detail {

// Native-image ingress implementation, not a graph-facing texture API.
class external_image_copy_s
{
  public:
    // Preconditions: compatible same-GPU producer, GENERAL foreign layout,
    // an exclusive borrow preventing reuse until this recording completes, and
    // producer readiness: Linux reservation fences, or completed GPU writes on Windows.
    // Linux checks reservation fences within the caller-provided budget before
    // enqueueing a GPU wait. Windows requires the qualified completion callback;
    // its NT texture handle carries no fence. Call only off the render thread.
    // Records the platform wait (if any), external acquire, draw, and release.
    // On failure abandon the recording. On success await the returned completion
    // before releasing the producer borrow; enqueue/submission is insufficient.
    static completion_s submit(recording_s&              record,
                               const external_image_s&   source,
                               const texture_s&          destination,
                               const draw_s&             conversion,
                               std::chrono::milliseconds readiness_timeout);
};

} // namespace miximus::gpu::detail
