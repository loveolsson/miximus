#pragma once

#include "external_image.hpp"

namespace miximus::gpu::detail {

// Creates an NT D3D11 texture on the Vulkan adapter, then imports the same storage.
std::shared_ptr<texture_state_s> create_external_image(const std::shared_ptr<device_state_s>& device, extent_s extent);

} // namespace miximus::gpu::detail
