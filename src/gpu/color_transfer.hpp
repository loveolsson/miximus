#pragma once
#include "types.hpp"

#include <array>

namespace miximus::gpu {

enum class color_transfer_e
{
    Rec601,
    Rec709,
};

constexpr std::array<float, 2> get_white_point(color_transfer_e c)
{
    switch (c) {
        case color_transfer_e::Rec601:
            return {0.2990f, 0.1140f};

        case color_transfer_e::Rec709:
        default:
            return {0.2126f, 0.0722f};
    }
}

constexpr mat3 get_color_transfer_from_yuv(color_transfer_e c)
{
    auto [wr, wb] = get_white_point(c);

    auto scale = [](float v) {
        int black = 16;
        int white = 235;
        int max   = 255;

        return (float)(1.0 * max / (white - black) * v);
    };

    return {
        {
         scale(1.0f),
         scale(1.0f),
         scale(1.0f),
         },
        {
         scale(0.0f),
         scale(-wb * (1.0f - wb) / 0.5f / (1 - wb - wr)),
         scale((1.0f - wb) / 0.5f),
         },
        {
         scale((1.0f - wr) / 0.5f),
         scale(-wr * (1 - wr) / 0.5f / (1 - wb - wr)),
         scale(0.0f),
         },
    };
}

constexpr mat3 get_color_transfer_to_yuv(color_transfer_e c)
{
    auto [wr, wb] = get_white_point(c);

    auto scale = [](float v) {
        int black = 16;
        int white = 235;
        int max   = 255;

        return (float)(1.0 * max / (white - black) * v);
    };

    return {
        {
         scale(wr),
         scale(1.0f - wb - wr),
         scale(wb),
         },
        {
         scale(-0.5f * wr / (1.0f - wb)),
         scale(-0.5f * (1 - wb - wr) / (1.0f - wb)),
         scale(0.5f),
         },
        {
         scale(0.5f),
         scale(-0.5f * (1.0f - wb - wr) / (1.0f - wr)),
         scale(-0.5f * wb / (1.0f - wr)),
         },
    };
}

} // namespace miximus::gpu