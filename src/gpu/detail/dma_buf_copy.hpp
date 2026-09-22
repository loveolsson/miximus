#pragma once

#include "dma_buf_image.hpp"
#include "gpu/recording.hpp"

#include <chrono>

namespace miximus::gpu::detail {

// Linux-only ingress implementation, not a graph-facing texture API.
class dma_buf_copy_s
{
  public:
    // Preconditions: compatible same-GPU producer, GENERAL foreign layout,
    // producer write fences published to the DMA-BUF reservation object, and
    // an exclusive borrow preventing reuse until this recording completes.
    // Checks producer readiness within the caller-provided budget before enqueueing
    // any external wait on the shared graphics queue. Call only off the render thread.
    // Records and enqueues a GPU fence wait, foreign acquire, draw, and release.
    // On failure abandon the recording. On success await the returned completion
    // before releasing the producer borrow; enqueue/submission is insufficient.
    static completion_s submit(recording_s&              record,
                               const dma_buf_image_s&    source,
                               const texture_s&          destination,
                               const draw_s&             conversion,
                               std::chrono::milliseconds readiness_timeout);
};

} // namespace miximus::gpu::detail
