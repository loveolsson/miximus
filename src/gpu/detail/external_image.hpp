#pragma once

#include "gpu/resource_types.hpp"
#include "native_handle.hpp"

#include <cstdint>
#include <memory>

namespace miximus::gpu::detail {

// Borrowed native texture: one-plane DMA-BUF on Linux, NT D3D11 texture on Windows.
// Import retains its own reference. Layout metadata applies to DMA-BUF only.
struct external_image_s
{
    native_handle_s::value_t handle{native_handle_s::invalid};
    extent_s                 extent;
    channel_order_e          order{channel_order_e::rgba};
    uint64_t                 modifier{};
    uint64_t                 offset{};
    uint64_t                 stride{};
};

// The caller must establish a compatible producer on the same physical GPU.
// DMA-BUF cannot encode adapter identity; Windows verifies it when opening the texture.
// Creates a sampled-only image with one mip level. Does not acquire external
// queue ownership or establish producer readiness. Those must precede any GPU
// read, and that read must finish before the producer may reuse the storage.
std::shared_ptr<texture_state_s> import_external_image(const std::shared_ptr<device_state_s>& device,
                                                       const external_image_s&                descriptor);

} // namespace miximus::gpu::detail
