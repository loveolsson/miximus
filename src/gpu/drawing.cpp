#include "drawing.hpp"

namespace miximus::gpu {

color_transform_s
color_parameters(const color_conversion_s& conversion, const mat3& gamut, color_conversion_direction_e direction)
{
    color_transform_s result;
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            // The original input shader used row-vector multiplication and a
            // transposed uniform upload. Output and gamut used column vectors.
            result.matrix.at((row * 4) + column) = direction == color_conversion_direction_e::to_yuv
                                                       ? conversion.matrix[row][column]
                                                       : conversion.matrix[column][row];
            result.gamut.at((row * 4) + column)  = gamut[row][column];
        }
    }

    result.offset = {conversion.offset.x, conversion.offset.y, conversion.offset.z, 0};
    return result;
}

} // namespace miximus::gpu
