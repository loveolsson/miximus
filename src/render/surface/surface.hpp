#pragma once
#include "gpu/texture.hpp"
#include "gpu/transfer/transfer.hpp"
#include "gpu/types.hpp"

#include <glm/vec4.hpp>

namespace miximus::render {

class surface_s
{
    const gpu::vec2i_t                         dimensions_;
    std::unique_ptr<gpu::transfer::transfer_i> transfer_;
    std::unique_ptr<gpu::texture_s>            texture_;

  public:
    using rgba_pixel_t = glm::vec<4, uint8_t>;
    using mono_pixel_t = uint8_t;

    surface_s(gpu::vec2i_t dim);
    ~surface_s() = default;

    auto* transfer() { return transfer_.get(); }
    auto* texture() { return texture_.get(); }
    auto  dimensions() const { return dimensions_; }

    rgba_pixel_t* ptr() { return reinterpret_cast<rgba_pixel_t*>(transfer_->ptr()); }
    void          copy(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, int src_pitch, gpu::vec2i_t pos);
    void          copy(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, int src_pitch, gpu::vec2i_t pos);
    void          alpha_blend(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, int src_pitch, gpu::vec2i_t pos);
    void          alpha_blend(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, int src_pitch, gpu::vec2i_t pos);
    void          clear(const rgba_pixel_t& color);
};

} // namespace miximus::render
