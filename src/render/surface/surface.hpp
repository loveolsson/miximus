#pragma once
#include "gpu/types.hpp"
#include "render/surface/strided_image_view.hpp"
#include "surface_pixel.hpp"

#include <cstddef>
#include <span>

namespace miximus::render {

class surface_s
{
  public:
    using pixel_t = premultiplied_rgba_pixel_s;

    // Transfer-backed callers use this preferred alignment for the fast path.
    // Surface correctness does not depend on it, so ordinary storage remains
    // valid as well.
    static constexpr size_t PREFERRED_DATA_ALIGNMENT = 32;

  private:
    const gpu::vec2i_t dimensions_;
    std::span<pixel_t> pixels_;

    pixel_t*       data() noexcept { return pixels_.data(); }
    const pixel_t* data() const noexcept { return pixels_.data(); }

  public:
    surface_s(gpu::vec2i_t dimensions, std::span<std::byte> storage);
    surface_s(gpu::vec2i_t dimensions, std::span<pixel_t> pixels);

    gpu::vec2i_t             dimensions() const noexcept { return dimensions_; }
    std::span<pixel_t>       pixels() noexcept { return pixels_; }
    std::span<const pixel_t> pixels() const noexcept { return pixels_; }

    // Surface storage is linear premultiplied RGBA. These overloads convert
    // explicitly described source representations before source-over.
    void source_over(const strided_image_view_s<straight_rgba_pixel_s>& source, gpu::vec2i_t position) noexcept;
    void source_over(const strided_image_view_s<srgb_premultiplied_bgra_pixel_s>& source,
                     gpu::vec2i_t                                                 position) noexcept;
    void source_over(const strided_image_view_s<coverage_pixel_t>& source,
                     gpu::vec2i_t                                  position,
                     straight_rgba_pixel_s                         color = {255, 255, 255, 255}) noexcept;
    void source_over(gpu::recti_s rect, straight_rgba_pixel_s color) noexcept;
    void source_over_ellipse(gpu::recti_s bounds, straight_rgba_pixel_s color) noexcept;

    // Replacement drawing takes pixels already in the canonical premultiplied
    // representation. Empty or invalid geometry is consistently a no-op.
    void clear(pixel_t color) noexcept;
    void fill(gpu::recti_s rect, pixel_t color) noexcept;
    void draw_rect(gpu::recti_s rect, pixel_t color, int thickness = 1) noexcept;
    void draw_line(gpu::vec2i_t from, gpu::vec2i_t to, pixel_t color, int thickness = 1) noexcept;

    void fill_ellipse(gpu::recti_s bounds, pixel_t color) noexcept;
    void draw_ellipse(gpu::recti_s bounds, pixel_t color, int thickness = 1) noexcept;
    void fill_circle(gpu::vec2i_t center, int radius, pixel_t color) noexcept;
    void draw_circle(gpu::vec2i_t center, int radius, pixel_t color, int thickness = 1) noexcept;
    void fill_pill(gpu::recti_s bounds, pixel_t color) noexcept;
    void draw_pill(gpu::recti_s bounds, pixel_t color, int thickness = 1) noexcept;

    void horizontal_gradient(gpu::recti_s rect, pixel_t left, pixel_t right) noexcept;
    void vertical_gradient(gpu::recti_s rect, pixel_t top, pixel_t bottom) noexcept;
    void bilinear_gradient(gpu::recti_s rect,
                           pixel_t      top_left,
                           pixel_t      top_right,
                           pixel_t      bottom_left,
                           pixel_t      bottom_right) noexcept;

    void checkerboard(gpu::recti_s rect, gpu::vec2i_t cell_size, pixel_t first, pixel_t second) noexcept;
    void draw_grid(gpu::recti_s rect, gpu::vec2i_t spacing, pixel_t color, int thickness = 1) noexcept;
};

} // namespace miximus::render
