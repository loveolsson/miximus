#pragma once

#include <cstdint>

namespace miximus::gpu {

// Describes how raw four-component texture storage is interpreted when sampled.
// Transfer backends move bytes unchanged; shaders apply this mapping.
enum class input_component_mapping_e : std::uint8_t
{
    identity           = 0,
    bgra_to_rgba       = 1,
    bgrx_to_rgba       = 2,
    argb_to_rgba       = 3,
};

// Describes how a logical RGBA shader result is arranged in raw readback storage.
enum class output_component_mapping_e : std::uint8_t
{
    identity           = 0,
    rgba_to_argb_bytes = 1,
    rgba_to_bgra_bytes = 2,
};

} // namespace miximus::gpu
