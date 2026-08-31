#pragma once

#include "gpu/component_mapping.hpp"
#include "gpu/transfer/texture_transfer.hpp"

namespace miximus::gpu::transfer::detail {

struct texture_transfer_plan_s
{
    host_frame_layout_s         host_layout;
    vec2i_t                     texture_dimensions{};
    texture_s::storage_format_e storage_format{texture_s::storage_format_e::rgba_unorm8};
    GLenum                      pixel_format{};
    GLenum                      pixel_type{};
    size_t                      storage_bytes_per_texel{};
    input_component_mapping_e   input_mapping{input_component_mapping_e::identity};
    output_component_mapping_e  output_mapping{output_component_mapping_e::identity};
};

texture_transfer_plan_s make_texture_transfer_plan(host_frame_layout_s host_layout);
size_t                  estimate_slot_memory_usage(const texture_transfer_plan_s& transfer_plan);
size_t slot_memory_usage(const texture_transfer_plan_s& transfer_plan, size_t backend_allocation_bytes);

} // namespace miximus::gpu::transfer::detail
