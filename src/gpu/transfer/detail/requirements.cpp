#include "requirements.hpp"

#include <bit>
#include <limits>
#include <stdexcept>

namespace miximus::gpu::transfer::detail {

void normalize_requirements(texture_transfer_requirements_s& requirements)
{
    if (requirements.dimensions.x <= 0 || requirements.dimensions.y <= 0 || requirements.byte_size == 0) {
        throw std::invalid_argument("invalid texture transfer requirements");
    }

    const auto minimum_row_stride = texture_s::host_row_byte_size(requirements.dimensions, requirements.format);
    if (requirements.row_stride == 0) {
        requirements.row_stride = minimum_row_stride;
    }
    const auto host_bytes_per_texel = texture_s::format_info(requirements.format).host_bytes_per_texel;
    if (requirements.row_stride < minimum_row_stride || requirements.row_stride % host_bytes_per_texel != 0) {
        throw std::invalid_argument("texture transfer row stride is invalid for its pixel format");
    }
    if (requirements.row_stride / host_bytes_per_texel > static_cast<size_t>(std::numeric_limits<GLint>::max())) {
        throw std::invalid_argument("texture transfer row length exceeds the OpenGL limit");
    }

    const auto height = static_cast<size_t>(requirements.dimensions.y);
    if (requirements.row_stride > std::numeric_limits<size_t>::max() / height ||
        requirements.byte_size < requirements.row_stride * height) {
        throw std::invalid_argument("texture transfer buffer is too small for its row stride");
    }

    if (requirements.address_alignment == 0) {
        requirements.address_alignment = 1;
    }
    if (!std::has_single_bit(requirements.address_alignment)) {
        throw std::invalid_argument("texture transfer address alignment must be a power of two");
    }
}

} // namespace miximus::gpu::transfer::detail
