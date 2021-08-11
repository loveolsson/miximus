#pragma once
#include "types.hpp"

namespace miximus::gpu {

enum class color_transfer_e
{
    Rec601,
    Rec709,
};

static mat3 get_color_transfer(color_transfer_e c)
{
    float wr, wb;

    // Convert to RGB using Rec.709 conversion matrix (see eq 26.7 in Poynton 2003)
    // r = Y + 1.5748 * Cr;
    // g = Y - 0.1873 * Cb - 0.4681 * Cr;
    // b = Y + 1.8556 * Cb;

    // return {
    //     {1.f, 1.f, 1.f},
    //     {1.8556f, -0.1873f, 0},
    //     {0, -0.4681f, 1.5748f},
    // };

    switch (c) {
        case color_transfer_e::Rec601:
            wr = 0.2990f;
            wb = 0.1140f;
            break;

        case color_transfer_e::Rec709:
            wr = 0.2126f;
            wb = 0.0722f;
            break;

        default:
            assert(false);
            break;
    }

    mat3 mat = {
        {1.0f, 0.0f, (1.0f - wr) / 0.5f},
        {1.0f, -wb * (1.0f - wb) / 0.5f / (1 - wb - wr), -wr * (1 - wr) / 0.5f / (1 - wb - wr)},
        {1.0f, (1.0f - wb) / 0.5f, 0.0f},
    };

    return mat;
}

} // namespace miximus::gpu