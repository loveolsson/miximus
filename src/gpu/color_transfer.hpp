#pragma once
#include "types.hpp"

#include <array>

namespace miximus::gpu {

enum class color_transfer_e
{
    Rec601,
    Rec709,
    Rec2020,
};

struct color_conversion_s
{
    mat3   matrix;
    vec3_t offset;
};

constexpr std::array<float, 2> get_white_point(color_transfer_e c)
{
    switch (c) {
        case color_transfer_e::Rec601:
            return {0.2990f, 0.1140f};

        case color_transfer_e::Rec709:
            return {0.2126f, 0.0722f};

        case color_transfer_e::Rec2020:
        default:
            return {0.2627f, 0.0593f};
    }
}

constexpr mat3 get_gamut_transfer_from_rec709(color_transfer_e c)
{
    if (c != color_transfer_e::Rec2020) {
        return {1.0f};
    }

    // Linear-light Rec.709/sRGB primaries to linear-light Rec.2020 primaries.
    return {
        {0.6274040f, 0.3292820f, 0.0433136f},
        {0.0690970f, 0.9195400f, 0.0113612f},
        {0.0163916f, 0.0880132f, 0.8955950f},
    };
}

constexpr mat3 get_gamut_transfer_to_rec709(color_transfer_e c)
{
    if (c != color_transfer_e::Rec2020) {
        return {1.0f};
    }

    // Linear-light Rec.2020 primaries to linear-light Rec.709/sRGB primaries.
    return {
        {1.6604910f,  -0.5876411f, -0.0728499f},
        {-0.1245505f, 1.1328999f,  -0.0083494f},
        {-0.0181508f, -0.1005789f, 1.1187297f },
    };
}

constexpr color_conversion_s get_color_transfer_from_yuv(color_transfer_e c)
{
    auto [wr, wb] = get_white_point(c);

    constexpr float max_code     = 1023.0f;
    constexpr float y_black      = 64.0f;
    constexpr float y_white      = 940.0f;
    constexpr float chroma_black = 64.0f;
    constexpr float chroma_white = 960.0f;
    constexpr float chroma_zero  = 512.0f;
    constexpr float y_scale      = max_code / (y_white - y_black);
    constexpr float chroma_scale = max_code / (chroma_white - chroma_black);

    return {
        .matrix =
            {
                     {y_scale, y_scale, y_scale},
                     {0.0f, chroma_scale * -wb * (1.0f - wb) / (0.5f * (1.0f - wb - wr)), chroma_scale * (1.0f - wb) / 0.5f},
                     {chroma_scale * (1.0f - wr) / 0.5f, chroma_scale * -wr * (1.0f - wr) / (0.5f * (1.0f - wb - wr)), 0.0f},
                     },
        .offset = {y_black / max_code, chroma_zero / max_code, chroma_zero / max_code},
    };
}

constexpr color_conversion_s get_color_transfer_to_yuv(color_transfer_e c)
{
    auto [wr, wb] = get_white_point(c);

    constexpr float max_code     = 1023.0f;
    constexpr float y_black      = 64.0f;
    constexpr float y_white      = 940.0f;
    constexpr float chroma_zero  = 512.0f;
    constexpr float chroma_range = 896.0f;
    constexpr float y_scale      = (y_white - y_black) / max_code;
    constexpr float chroma_scale = chroma_range / max_code;

    return {
        .matrix =
            {
                     {y_scale * wr, y_scale * (1.0f - wb - wr), y_scale * wb},
                     {chroma_scale * -0.5f * wr / (1.0f - wb),
                 chroma_scale * -0.5f * (1.0f - wb - wr) / (1.0f - wb),
                 chroma_scale * 0.5f},
                     {chroma_scale * 0.5f,
                 chroma_scale * -0.5f * (1.0f - wb - wr) / (1.0f - wr),
                 chroma_scale * -0.5f * wb / (1.0f - wr)},
                     },
        .offset = {y_black / max_code, chroma_zero / max_code, chroma_zero / max_code},
    };
}

} // namespace miximus::gpu
