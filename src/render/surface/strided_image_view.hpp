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

    const std::byte* first_row_{};
    gpu::vec2i_t     dimensions_{};
    ptrdiff_t        row_stride_{};

    strided_image_view_s(const Pixel* first_row, gpu::vec2i_t dimensions, ptrdiff_t row_stride) noexcept
        : first_row_(reinterpret_cast<const std::byte*>(first_row))
        , dimensions_(dimensions)
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
    [[nodiscard]] static strided_image_view_s packed(std::span<const Pixel> pixels, gpu::vec2i_t dimensions)
    {
        const auto row_stride   = checked_row_size(dimensions);
        const auto storage_size = required_size(dimensions, row_stride);
        if (storage_size > static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max())) {
            throw std::length_error("image view size overflows ptrdiff_t");
        }
        if (std::as_bytes(pixels).size() < storage_size) {
            throw std::invalid_argument("image storage is smaller than its dimensions");
        }
        return {pixels.data(), dimensions, static_cast<ptrdiff_t>(row_stride)};
    }

    // Adapter for external APIs that expose the logical first row as a pointer
    // and use a signed byte stride for subsequent rows.
    [[nodiscard]] static strided_image_view_s
    from_rows(const Pixel* first_row, gpu::vec2i_t dimensions, ptrdiff_t row_stride)
    {
        if (row_stride == std::numeric_limits<ptrdiff_t>::min()) {
            throw std::invalid_argument("image row stride is not representable");
        }
        const auto absolute_stride = static_cast<size_t>(row_stride < 0 ? -row_stride : row_stride);
        if (absolute_stride % alignof(Pixel) != 0) {
            throw std::invalid_argument("image row stride does not preserve pixel alignment");
        }
        const auto storage_size = required_size(dimensions, absolute_stride);
        if (storage_size > static_cast<size_t>(std::numeric_limits<ptrdiff_t>::max())) {
            throw std::length_error("image view size overflows ptrdiff_t");
        }
        if (storage_size > 0 && first_row == nullptr) {
            throw std::invalid_argument("image storage must not be null");
        }
        return {first_row, dimensions, row_stride};
    }

    gpu::vec2i_t dimensions() const noexcept { return dimensions_; }
    ptrdiff_t    row_stride_bytes() const noexcept { return row_stride_; }

    // Construction validates the complete storage range. Raster operations
    // clip row indices before using this unchecked hot-path accessor.
    std::span<const Pixel> row(size_t y) const noexcept
    {
        const auto  byte_offset = static_cast<ptrdiff_t>(y) * row_stride_;
        const auto* pixels      = reinterpret_cast<const Pixel*>(first_row_ + byte_offset);
        return {pixels, static_cast<size_t>(dimensions_.x)};
    }
};

} // namespace miximus::render
