#pragma once
#include "gpu/types.hpp"

#include <cstddef>
#include <cstdint>
#include <glm/vec4.hpp>
#include <memory>

namespace miximus::render {

class surface_s
{
  public:
    using rgba_pixel_t = glm::vec<4, uint8_t>;
    using mono_pixel_t = uint8_t;

    static constexpr size_t DATA_ALIGNMENT = 32;

  private:
    const gpu::vec2i_t dimensions_;
    rgba_pixel_t*      ptr_;

  public:
    surface_s(gpu::vec2i_t dim, rgba_pixel_t* ptr);

    auto                dimensions() const { return dimensions_; }
    rgba_pixel_t*       ptr() { return std::assume_aligned<DATA_ALIGNMENT>(ptr_); }
    const rgba_pixel_t* ptr() const { return std::assume_aligned<DATA_ALIGNMENT>(ptr_); }

    [[nodiscard]] bool  contains(gpu::vec2i_t position) const;
    rgba_pixel_t&       pixel(gpu::vec2i_t position);
    const rgba_pixel_t& pixel(gpu::vec2i_t position) const;

    void copy(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos);
    void copy(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos);
    void alpha_blend(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos);
    void alpha_blend(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos);
    void source_over(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos);
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
