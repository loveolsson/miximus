#pragma once

#include "gpu/recording.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace miximus::gpu {

struct color_primaries_s
{
    vec2_t red;
    vec2_t green;
    vec2_t blue;
    vec2_t white;
};

// Returns rows in the same convention as get_gamut_transfer_to_rec709().
mat3 video_gamut_to_rec709(const color_primaries_s& source);

// Source transfer function, independent of YUV matrix and RGB primaries.
enum class video_transfer_e : uint32_t
{
    rec709,
    srgb,
    linear,
    gamma22,
    gamma28
};

// Integer component addressing works for planar, semi-planar and packed RGB/YUV.
// Offsets address bytes in one bounded component buffer. Values are normalized by
// their actual bit depth, including high-bit-aligned formats such as P010.
struct video_component_s
{
    uint32_t offset{};
    uint32_t stride{};
    uint32_t step{};
    uint32_t encoding{};
};

struct video_buffer_header_s
{
    std::array<video_component_s, 4> components{};
    float                            chroma_offset_x{};
    float                            chroma_offset_y{};
    // Premultiplication in source (encoded) RGB must be undone before transfer
    // decoding, then applied again in the linear working space. Unspecified
    // video alpha is treated as straight.
    uint32_t encoded_premultiplied_alpha{};
    uint32_t component_count{};
};

static_assert(sizeof(video_buffer_header_s) == 80);

// Encoding layout shared with unpack_video.comp: depth, shift, subsampling x/y,
// big endian. Descriptor construction rejects unsupported component layouts.
uint32_t
video_component_encoding(unsigned depth, unsigned shift, unsigned subsample_x, unsigned subsample_y, bool big_endian);

} // namespace miximus::gpu
