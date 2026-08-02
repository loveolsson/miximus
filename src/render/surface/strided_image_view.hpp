#pragma once
#include "gpu/types.hpp"

#include <cstddef>
#include <limits>
#include <span>
#include <stdexcept>
#include <type_traits>

namespace miximus::render {

template <typename Pixel>
class strided_image_view_s
{
    static_assert(std::is_trivially_copyable_v<Pixel>);

    std::span<const std::byte> storage_;
    gpu::vec2i_t               dimensions_{};
    size_t                     row_zero_offset_{};
    ptrdiff_t                  row_stride_{};

    strided_image_view_s(std::span<const std::byte> storage,
                         gpu::vec2i_t               dimensions,
                         size_t                     row_zero_offset,
                         ptrdiff_t                  row_stride) noexcept
        : storage_(storage)
        , dimensions_(dimensions)
        , row_zero_offset_(row_zero_offset)
        , row_stride_(row_stride)
    {
    }

    static size_t checked_row_size(gpu::vec2i_t dimensions)
    {
        if (dimensions.x < 0 || dimensions.y < 0) {
            throw std::invalid_argument("image dimensions must not be negative");
        }
        if (static_cast<size_t>(dimensions.x) > std::numeric_limits<size_t>::max() / sizeof(Pixel)) {
            throw std::length_error("image row size overflows size_t");
        }
        return static_cast<size_t>(dimensions.x) * sizeof(Pixel);
    }

    static size_t required_size(gpu::vec2i_t dimensions, size_t row_stride)
    {
        const auto row_size = checked_row_size(dimensions);
        if (dimensions.y == 0 || row_size == 0) {
            return 0;
        }
        if (row_stride < row_size) {
            throw std::invalid_argument("image row stride is smaller than one row");
        }
        const auto remaining_rows = static_cast<size_t>(dimensions.y - 1);
        if (remaining_rows > (std::numeric_limits<size_t>::max() - row_size) / row_stride) {
            throw std::length_error("image view size overflows size_t");
        }
        return (remaining_rows * row_stride) + row_size;
    }

  public:
    static strided_image_view_s packed(std::span<const Pixel> pixels, gpu::vec2i_t dimensions)
    {
        const auto row_stride = checked_row_size(dimensions);
        if (row_stride > static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max())) {
            throw std::length_error("image row stride overflows ptrdiff_t");
        }
        const auto bytes = std::as_bytes(pixels);
        if (bytes.size() < required_size(dimensions, row_stride)) {
            throw std::invalid_argument("image storage is smaller than its dimensions");
        }
        return {bytes, dimensions, 0, static_cast<ptrdiff_t>(row_stride)};
    }

    // Adapter for external APIs that expose the logical first row as a pointer
    // and use a signed byte stride for subsequent rows.
    static strided_image_view_s from_rows(const Pixel* first_row, gpu::vec2i_t dimensions, ptrdiff_t row_stride)
    {
        if (row_stride == std::numeric_limits<ptrdiff_t>::min()) {
            throw std::invalid_argument("image row stride is not representable");
        }
        const auto absolute_stride = static_cast<size_t>(row_stride < 0 ? -row_stride : row_stride);
        const auto storage_size    = required_size(dimensions, absolute_stride);
        if (storage_size > static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max())) {
            throw std::length_error("image view size overflows ptrdiff_t");
        }
        if (storage_size > 0 && first_row == nullptr) {
            throw std::invalid_argument("image storage must not be null");
        }
        if (storage_size == 0) {
            return {{}, dimensions, 0, row_stride};
        }
        const auto row_zero_offset =
            row_stride < 0 && dimensions.y > 0 ? absolute_stride * static_cast<size_t>(dimensions.y - 1) : 0;
        const auto* storage = reinterpret_cast<const std::byte*>(first_row) - row_zero_offset;
        return {
            {storage, storage_size},
            dimensions,
            row_zero_offset,
            row_stride,
        };
    }

    gpu::vec2i_t dimensions() const noexcept { return dimensions_; }
    ptrdiff_t    row_stride_bytes() const noexcept { return row_stride_; }

    // Construction validates the complete storage range. Raster operations
    // clip row indices before using this unchecked hot-path accessor.
    std::span<const Pixel> row(size_t y) const noexcept
    {
        const auto offset = static_cast<ptrdiff_t>(row_zero_offset_) + (static_cast<ptrdiff_t>(y) * row_stride_);
        return {reinterpret_cast<const Pixel*>(storage_.data() + offset), static_cast<size_t>(dimensions_.x)};
    }
};

} // namespace miximus::render
