#pragma once

#include "external_image.hpp"

#include <d3d11.h>
#include <volk.h>
#include <wrl/client.h>

namespace miximus::gpu::detail {

// Internal raw-image import shared by browser RGB and video plane ingress.
// Transfer-only images have no sampled view and must never become graph textures.
struct d3d11_image_description_s
{
    native_handle_s::value_t handle{native_handle_s::invalid};
    extent_s                 extent;
    DXGI_FORMAT              dxgi_format{};
    VkFormat                 format{};
    VkImageUsageFlags        usage{};
};

std::shared_ptr<texture_state_s> import_d3d11_image(const std::shared_ptr<device_state_s>& device,
                                                    const d3d11_image_description_s&       descriptor);

Microsoft::WRL::ComPtr<ID3D11Device> create_d3d11_producer_device(const std::shared_ptr<device_state_s>& owner);

// Creates an NT D3D11 texture on the Vulkan adapter, then imports the same storage.
std::shared_ptr<texture_state_s> create_external_image(const std::shared_ptr<device_state_s>& device, extent_s extent);

} // namespace miximus::gpu::detail
