#pragma once

#include "gpu/resource_types.hpp"
#include "gpu/transfer/texture_transfer.hpp"

namespace miximus::gpu::transfer::detail {

struct texture_transfer_plan_s
{
    host_frame_layout_s host_layout;
    size_t              packed_buffer_bytes{};

    size_t          storage_bytes_per_texel{};
    channel_order_e input_order{channel_order_e::rgba};
    channel_order_e output_order{channel_order_e::rgba};
};

[[nodiscard]] texture_transfer_plan_s make_texture_transfer_plan(host_frame_layout_s host_layout);
[[nodiscard]] size_t estimate_slot_memory_usage(const texture_transfer_plan_s& transfer_plan, sampling_e sampling);
[[nodiscard]] size_t
slot_memory_usage(const texture_transfer_plan_s& transfer_plan, size_t backend_allocation_bytes, sampling_e sampling);

} // namespace miximus::gpu::transfer::detail
