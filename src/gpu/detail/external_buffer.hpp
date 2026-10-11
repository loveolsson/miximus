#pragma once

#include "gpu/device.hpp"
#include "native_handle.hpp"

#include <array>

namespace miximus::gpu::detail {

struct external_buffer_description_s
{
    native_handle_s::value_t handle{native_handle_s::invalid};
    size_t                   bytes{};
    size_t                   allocation_bytes{};
    uint32_t                 memory_type{};
    std::array<uint8_t, 16>  device_uuid{};
    std::array<uint8_t, 16>  driver_uuid{};
};

// An exclusive, completed foreign GPU write, like the existing Windows image
// ingress contract. The producer must not reuse storage until conversion completes.
class external_buffer_s
{
    buffer_s buffer_;

  public:
    external_buffer_s(device_s& device, const external_buffer_description_s& description);
    completion_s convert(recording_s& record, const texture_s& target, const color_transform_s& color);
};
} // namespace miximus::gpu::detail
