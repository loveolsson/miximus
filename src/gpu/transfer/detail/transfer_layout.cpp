#include "transfer_layout.hpp"

#include <bit>
#include <limits>
#include <stdexcept>

namespace miximus::gpu::transfer::detail {
namespace {
size_t checked_add(size_t lhs, size_t rhs)
{
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        throw std::overflow_error("texture transfer allocation size overflow");
    }
    return lhs + rhs;
}

size_t checked_multiply(size_t lhs, size_t rhs)
{
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        throw std::overflow_error("texture transfer allocation size overflow");
    }
    return lhs * rhs;
}
} // namespace

void normalize_transfer_layout(texture_transfer_layout_s& transfer_layout)
{
    if (transfer_layout.dimensions.x <= 0 || transfer_layout.dimensions.y <= 0 ||
        transfer_layout.host_buffer_size_bytes == 0) {
        throw std::invalid_argument("invalid texture transfer layout");
    }

    const auto minimum_row_stride =
        texture_s::host_row_byte_size(transfer_layout.dimensions, transfer_layout.pixel_format);
    if (transfer_layout.host_row_stride_bytes == 0) {
        transfer_layout.host_row_stride_bytes = minimum_row_stride;
    }
    const auto host_bytes_per_texel = texture_s::pixel_format_info(transfer_layout.pixel_format).host_bytes_per_texel;
    if (transfer_layout.host_row_stride_bytes < minimum_row_stride ||
        transfer_layout.host_row_stride_bytes % host_bytes_per_texel != 0) {
        throw std::invalid_argument("texture transfer row stride is invalid for its pixel format");
    }
    if (transfer_layout.host_row_stride_bytes / host_bytes_per_texel >
        static_cast<size_t>(std::numeric_limits<GLint>::max())) {
        throw std::invalid_argument("texture transfer row length exceeds the OpenGL limit");
    }

    const auto height = static_cast<size_t>(transfer_layout.dimensions.y);
    if (transfer_layout.host_row_stride_bytes > std::numeric_limits<size_t>::max() / height ||
        transfer_layout.host_buffer_size_bytes < transfer_layout.host_row_stride_bytes * height) {
        throw std::invalid_argument("texture transfer buffer is too small for its row stride");
    }

    if (transfer_layout.host_address_alignment_bytes == 0) {
        transfer_layout.host_address_alignment_bytes = 1;
    }
    if (!std::has_single_bit(transfer_layout.host_address_alignment_bytes)) {
        throw std::invalid_argument("texture transfer address alignment must be a power of two");
    }
}

size_t estimate_slot_memory_usage(const texture_transfer_layout_s& transfer_layout)
{
    // CUDA may own both pinned host storage and an interop PBO. Other
    // asynchronous backends use no more, so this is a conservative cap.
    return checked_add(checked_multiply(transfer_layout.host_buffer_size_bytes, 2),
                       texture_s::estimate_storage_byte_size(transfer_layout.dimensions, transfer_layout.pixel_format));
}

size_t slot_memory_usage(const texture_transfer_layout_s& transfer_layout, size_t backend_allocation_bytes)
{
    return checked_add(backend_allocation_bytes,
                       texture_s::estimate_storage_byte_size(transfer_layout.dimensions, transfer_layout.pixel_format));
}

} // namespace miximus::gpu::transfer::detail
