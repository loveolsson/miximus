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

texture_transfer_plan_s make_texture_transfer_plan(host_frame_layout_s host_layout)
{
    if (host_layout.image_dimensions.x <= 0 || host_layout.image_dimensions.y <= 0 ||
        host_layout.buffer_size_bytes == 0) {
        throw std::invalid_argument("invalid texture transfer layout");
    }

    constexpr size_t bytes_per_storage_texel = 4;
    size_t           minimum_row_stride{};
    switch (host_layout.pixel_format) {
        case host_pixel_format_e::rgba_u8:
        case host_pixel_format_e::bgra_u8:
        case host_pixel_format_e::bgrx_u8:
        case host_pixel_format_e::argb_u8:
            minimum_row_stride = checked_multiply(static_cast<size_t>(host_layout.image_dimensions.x), 4);
            break;
        case host_pixel_format_e::v210:
            if (host_layout.row_stride_bytes == 0 || host_layout.row_stride_bytes % bytes_per_storage_texel != 0) {
                throw std::invalid_argument("v210 transfer row stride must contain whole 32-bit words");
            }
            minimum_row_stride = checked_multiply((static_cast<size_t>(host_layout.image_dimensions.x) + 5) / 6, 16);
            break;
    }
    if (host_layout.row_stride_bytes == 0) {
        host_layout.row_stride_bytes = minimum_row_stride;
    }
    if (host_layout.row_stride_bytes < minimum_row_stride ||
        host_layout.row_stride_bytes % bytes_per_storage_texel != 0) {
        throw std::invalid_argument("texture transfer row stride is invalid for its pixel format");
    }

    if (host_layout.row_stride_bytes / bytes_per_storage_texel > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::invalid_argument("texture transfer row length exceeds the supported limit");
    }

    const auto height = static_cast<size_t>(host_layout.image_dimensions.y);
    if (host_layout.row_stride_bytes > std::numeric_limits<size_t>::max() / height ||
        host_layout.buffer_size_bytes < host_layout.row_stride_bytes * height) {
        throw std::invalid_argument("texture transfer buffer is too small for its row stride");
    }

    if (host_layout.address_alignment_bytes == 0) {
        host_layout.address_alignment_bytes = 1;
    }
    if (!std::has_single_bit(host_layout.address_alignment_bytes)) {
        throw std::invalid_argument("texture transfer address alignment must be a power of two");
    }

    texture_transfer_plan_s result{
        .host_layout             = host_layout,
        .storage_bytes_per_texel = bytes_per_storage_texel,
    };
    switch (host_layout.pixel_format) {
        case host_pixel_format_e::rgba_u8:
            break;
        case host_pixel_format_e::bgra_u8:
            result.input_order  = channel_order_e::bgra;
            result.output_order = channel_order_e::bgra;
            break;
        case host_pixel_format_e::bgrx_u8:
            result.input_order = channel_order_e::bgrx;
            break;
        case host_pixel_format_e::argb_u8:
            result.input_order  = channel_order_e::argb;
            result.output_order = channel_order_e::argb;
            break;
        case host_pixel_format_e::v210:
            result.packed_buffer_bytes = checked_multiply(host_layout.row_stride_bytes, height);
            break;
    }
    return result;
}

size_t estimate_slot_memory_usage(const texture_transfer_plan_s& transfer_plan, sampling_e sampling)
{
    // Reserve a conservative allowance for host-buffer allocation padding.
    return checked_add(checked_multiply(transfer_plan.host_layout.buffer_size_bytes, 2),
                       (transfer_plan.packed_buffer_bytes != 0
                            ? transfer_plan.packed_buffer_bytes
                            : texture_s::estimate_storage_byte_size(
                                  transfer_plan.host_layout.image_dimensions, format_e::rgba_unorm8, sampling)));
}

size_t
slot_memory_usage(const texture_transfer_plan_s& transfer_plan, size_t backend_allocation_bytes, sampling_e sampling)
{
    return checked_add(backend_allocation_bytes,
                       (transfer_plan.packed_buffer_bytes != 0
                            ? transfer_plan.packed_buffer_bytes
                            : texture_s::estimate_storage_byte_size(
                                  transfer_plan.host_layout.image_dimensions, format_e::rgba_unorm8, sampling)));
}

} // namespace miximus::gpu::transfer::detail
