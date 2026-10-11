#include "video_frame.hpp"

#include <cmath>
#include <glm/matrix.hpp>
#include <stdexcept>

namespace miximus::gpu {

namespace {

glm::dvec3 xyz(vec2_t xy)
{
    if (!std::isfinite(xy.x) || !std::isfinite(xy.y) || xy.y <= 0.0) {
        throw std::invalid_argument("Invalid video primary chromaticity");
    }

    return {xy.x / xy.y, 1.0, (1.0 - xy.x - xy.y) / xy.y};
}

glm::dmat3 rgb_to_xyz(const color_primaries_s& primaries)
{
    glm::dmat3 basis{xyz(primaries.red), xyz(primaries.green), xyz(primaries.blue)};
    if (std::abs(glm::determinant(basis)) < 1e-12) {
        throw std::invalid_argument("Singular video primaries");
    }

    const auto scale = glm::inverse(basis) * xyz(primaries.white);
    return {basis[0] * scale.x, basis[1] * scale.y, basis[2] * scale.z};
}
} // namespace

mat3 video_gamut_to_rec709(const color_primaries_s& source)
{
    const color_primaries_s destination{
        .red   = {0.64,   0.33  },
        .green = {0.30,   0.60  },
        .blue  = {0.15,   0.06  },
        .white = {0.3127, 0.3290},
    };

    // Bradford adaptation is identity for the common D65-to-D65 case.
    const glm::dmat3 bradford{
        {0.8951,  -0.7502, 0.0389 },
        {0.2664,  1.7135,  -0.0685},
        {-0.1614, 0.0367,  1.0296 }
    };

    const auto source_white = bradford * xyz(source.white);
    const auto target_white = bradford * xyz(destination.white);
    glm::dmat3 scale{1.0};
    for (int i = 0; i < 3; ++i) {
        if (std::abs(source_white[i]) < 1e-12) {
            throw std::invalid_argument("Invalid video white point");
        }

        scale[i][i] = target_white[i] / source_white[i];
    }

    const auto adaptation = glm::inverse(bradford) * scale * bradford;
    return mat3{glm::transpose(glm::inverse(rgb_to_xyz(destination)) * adaptation * rgb_to_xyz(source))};
}

uint32_t
video_component_encoding(unsigned depth, unsigned shift, unsigned subsample_x, unsigned subsample_y, bool big_endian)
{
    if (depth == 0 || depth > 16 || depth + shift > 32 || subsample_x > 2 || subsample_y > 2) {
        throw std::invalid_argument("Unsupported video component layout");
    }

    return depth | (shift << 8) | (subsample_x << 16) | (subsample_y << 20) | (static_cast<uint32_t>(big_endian) << 24);
}
} // namespace miximus::gpu
