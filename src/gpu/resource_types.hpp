#pragma once

#include <cstdint>

namespace miximus::gpu {
namespace transfer::detail {

class cuda_transfer_s;
}

namespace detail {

class dma_buf_copy_s;
class color_comparison_s;
struct device_state_s;
struct texture_state_s;
struct buffer_state_s;
struct recording_state_s;
struct recording_context_state_s;
struct submission_state_s;
struct presenter_state_s;
} // namespace detail

struct extent_s
{
    uint32_t width{};
    uint32_t height{};
    bool     operator==(const extent_s&) const = default;
};

// Transfer images use raw RGBA8; channel order and video packing belong in shaders.
// Four-channel UNORM16 working targets retain precision without packed RGB formats.
enum class format_e
{
    rgba_unorm8,
    rgba_unorm16,
    rgba16_float,
    r32_uint
};

enum class channel_order_e : uint32_t
{
    rgba,
    bgra,
    bgrx,
    argb
};

enum class sampling_e
{
    nearest,
    linear,
    mipmapped_linear
};

// Export only resources shared with an external transfer API. Working targets stay local.
enum class resource_sharing_e
{
    local,
    cuda
};

enum class host_access_e
{
    sequential_write,
    read_write,
    readback,
    device_only
};

enum class wait_result_e
{
    ready,
    timeout,
    cancelled
};

} // namespace miximus::gpu
