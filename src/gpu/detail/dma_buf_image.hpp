#pragma once

#include "gpu/resource_types.hpp"

#include <cstdint>
#include <memory>

namespace miximus::gpu::detail {

// Linux implementation boundary. The descriptor borrows its FD; importing
// duplicates it. This first path accepts one DRM memory plane only.
struct dma_buf_image_s
{
    int             fd{-1};
    extent_s        extent;
    channel_order_e order{channel_order_e::rgba};
    uint64_t        modifier{};
    uint64_t        offset{};
    uint64_t        stride{};
};

// The caller must establish that the FD is a DMA-BUF from a compatible producer
// on the same physical GPU. This function cannot infer adapter identity from an FD.
// Creates a sampled-only image with one mip level. Does not acquire foreign
// queue ownership or establish producer readiness. Those must precede any GPU
// read, and that read must finish before the producer may reuse the storage.
std::shared_ptr<texture_state_s> import_dma_buf_image(const std::shared_ptr<device_state_s>& device,
                                                      const dma_buf_image_s&                 descriptor);

} // namespace miximus::gpu::detail
