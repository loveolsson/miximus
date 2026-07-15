#pragma once
#include "gpu/types.hpp"

#include <cstdint>
#include <glm/vec4.hpp>

namespace miximus::render {

class surface_s
{
  public:
    using rgba_pixel_t = glm::vec<4, uint8_t>;
    using mono_pixel_t = uint8_t;

  private:
    const gpu::vec2i_t dimensions_;
    rgba_pixel_t*      ptr_;

  public:
    surface_s(gpu::vec2i_t dim, rgba_pixel_t* ptr);

    auto dimensions() const { return dimensions_; }
    auto ptr() { return ptr_; }
    void copy(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos);
    void copy(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos);
    void alpha_blend(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos);
    void alpha_blend(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos);
    void clear(const rgba_pixel_t& color);
};

} // namespace miximus::render
