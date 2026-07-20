#pragma once
#include "gpu/types.hpp"

#include <cstddef>
#include <cstdint>
#include <glm/vec4.hpp>
#include <limits>
#include <memory>
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
                         ptrdiff_t                  row_stride)
        : storage_(storage)
        , dimensions_(dimensions)
        , row_zero_offset_(row_zero_offset)
        , row_stride_(row_stride)
    {
    }

    static size_t required_size(gpu::vec2i_t dimensions, size_t row_stride)
    {
        if (dimensions.x < 0 || dimensions.y < 0) {
            throw std::invalid_argument("image dimensions must not be negative");
        }
        if (static_cast<size_t>(dimensions.x) > std::numeric_limits<size_t>::max() / sizeof(Pixel)) {
            throw std::length_error("image row size overflows size_t");
        }
        const auto row_size = static_cast<size_t>(dimensions.x) * sizeof(Pixel);
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
        if (dimensions.x < 0 || dimensions.y < 0) {
            throw std::invalid_argument("image dimensions must not be negative");
        }
        if (static_cast<size_t>(dimensions.x) > std::numeric_limits<size_t>::max() / sizeof(Pixel)) {
            throw std::length_error("image row size overflows size_t");
        }
        const auto row_stride = static_cast<size_t>(dimensions.x) * sizeof(Pixel);
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
            dimensions, row_zero_offset, row_stride
        };
    }

    gpu::vec2i_t dimensions() const noexcept { return dimensions_; }
    ptrdiff_t    row_stride_bytes() const noexcept { return row_stride_; }

    // Construction validates the complete storage range. Raster operations
    // clip their row indices once before entering the hot loop.
    std::span<const Pixel> row(size_t y) const noexcept
    {
        const auto offset = static_cast<ptrdiff_t>(row_zero_offset_) + (static_cast<ptrdiff_t>(y) * row_stride_);
        return {reinterpret_cast<const Pixel*>(storage_.data() + offset), static_cast<size_t>(dimensions_.x)};
    }
};

class surface_s
{
  public:
    using rgba_pixel_t = glm::vec<4, uint8_t>;
    using mono_pixel_t = uint8_t;

    static constexpr size_t DATA_ALIGNMENT = 32;

  private:
    const gpu::vec2i_t      dimensions_;
    std::span<rgba_pixel_t> pixels_;

    rgba_pixel_t*       aligned_data() { return std::assume_aligned<DATA_ALIGNMENT>(pixels_.data()); }
    const rgba_pixel_t* aligned_data() const { return std::assume_aligned<DATA_ALIGNMENT>(pixels_.data()); }

  public:
    surface_s(gpu::vec2i_t dimensions, std::span<std::byte> storage);
    surface_s(gpu::vec2i_t dimensions, std::span<rgba_pixel_t> pixels);

    auto                          dimensions() const { return dimensions_; }
    std::span<rgba_pixel_t>       pixels() { return pixels_; }
    std::span<const rgba_pixel_t> pixels() const { return pixels_; }

    [[nodiscard]] bool  contains(gpu::vec2i_t position) const;
    rgba_pixel_t&       pixel(gpu::vec2i_t position);
    const rgba_pixel_t& pixel(gpu::vec2i_t position) const;

    void copy(const strided_image_view_s<rgba_pixel_t>& source, gpu::vec2i_t position);
    void copy(const strided_image_view_s<mono_pixel_t>& source, gpu::vec2i_t position);
    void alpha_blend(const strided_image_view_s<rgba_pixel_t>& source, gpu::vec2i_t position);
    void alpha_blend(const strided_image_view_s<mono_pixel_t>& source, gpu::vec2i_t position);
    void source_over(const strided_image_view_s<rgba_pixel_t>& source, gpu::vec2i_t position);
    void source_over(gpu::recti_s rect, const rgba_pixel_t& color);
    void source_over_ellipse(gpu::recti_s bounds, const rgba_pixel_t& color);

    void clear(const rgba_pixel_t& color);
    void fill(gpu::recti_s rect, const rgba_pixel_t& color);
    void draw_rect(gpu::recti_s rect, const rgba_pixel_t& color, int thickness = 1);
    void draw_line(gpu::vec2i_t from, gpu::vec2i_t to, const rgba_pixel_t& color, int thickness = 1);

    void fill_ellipse(gpu::recti_s bounds, const rgba_pixel_t& color);
    void draw_ellipse(gpu::recti_s bounds, const rgba_pixel_t& color, int thickness = 1);
    void fill_circle(gpu::vec2i_t center, int radius, const rgba_pixel_t& color);
    void draw_circle(gpu::vec2i_t center, int radius, const rgba_pixel_t& color, int thickness = 1);
    void fill_pill(gpu::recti_s bounds, const rgba_pixel_t& color);
    void draw_pill(gpu::recti_s bounds, const rgba_pixel_t& color, int thickness = 1);

    void horizontal_gradient(gpu::recti_s rect, const rgba_pixel_t& left, const rgba_pixel_t& right);
    void vertical_gradient(gpu::recti_s rect, const rgba_pixel_t& top, const rgba_pixel_t& bottom);
    void bilinear_gradient(gpu::recti_s        rect,
                           const rgba_pixel_t& top_left,
                           const rgba_pixel_t& top_right,
                           const rgba_pixel_t& bottom_left,
                           const rgba_pixel_t& bottom_right);

    void checkerboard(gpu::recti_s rect, gpu::vec2i_t cell_size, const rgba_pixel_t& first, const rgba_pixel_t& second);
    void draw_grid(gpu::recti_s rect, gpu::vec2i_t spacing, const rgba_pixel_t& color, int thickness = 1);
};

} // namespace miximus::render
