#pragma once

#include <cstdint>

namespace miximus::gpu::transfer {

// Describes how a final readback shader must arrange its logical RGBA result
// when the selected backend copies raw texture storage instead of applying the
// texture's external OpenGL format and type.
enum class readback_component_mapping_e : std::uint8_t
{
    identity           = 0,
    rgba_to_argb_bytes = 1,
    rgba_to_bgra_bytes = 2,
};

} // namespace miximus::gpu::transfer
