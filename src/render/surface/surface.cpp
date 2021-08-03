#include "surface.hpp"
#include <algorithm>
#include <glm/glm.hpp>

namespace miximus::render {

surface_s::surface_s(gpu::vec2i_t dim)
    : dimensions_(dim)
{
    using transfer_i = gpu::transfer::transfer_i;
    size_t size      = dim.x * dim.y * sizeof(rgba_pixel_t);

    texture_  = std::make_unique<gpu::texture_s>(dim, gpu::texture_s::colorspace_e::RGBA);
    transfer_ = transfer_i::create_transfer(transfer_i::get_prefered_type(), size, transfer_i::direction_e::cpu_to_gpu);
}

void surface_s::clear(const rgba_pixel_t& color) { std::fill(ptr(), ptr() + dimensions_.x * dimensions_.y, color); }

template <typename SrcT, typename Op>
static inline void copy_operation(const SrcT*              src_ptr,
                                  gpu::vec2i_t             src_dim,
                                  int                      src_pitch,
                                  surface_s::rgba_pixel_t* dst_ptr,
                                  gpu::vec2i_t             dst_dim,
                                  gpu::vec2i_t             pos,
                                  Op                       op)
{
    for (int sy = 0; sy < src_dim.y; ++sy) {
        int dy = pos.y + sy;

        if (dy < 0 || dy >= dst_dim.y) {
            continue;
        }

        for (int sx = 0; sx < src_dim.x; ++sx) {
            int dx = pos.x + sx;
            if (dx < 0 || dx >= dst_dim.x) {
                continue;
            }

            const auto* sp = &src_ptr[src_dim.x * sy + sx];
            auto*       dp = &dst_ptr[dst_dim.x * dy + dx];

            op(*(sp++), *(dp++));
        }
    }
}

void surface_s::copy(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, int src_pitch, gpu::vec2i_t pos)
{
    auto op = [](auto& src, auto& dst) { dst = src; };
    copy_operation(src_ptr, src_dim, src_dim.x, ptr(), dimensions_, pos, op);
}

void surface_s::copy(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, int src_pitch, gpu::vec2i_t pos)
{
    auto op = [](auto& src, auto& dst) { dst = {src, src, src, src}; };
    copy_operation(src_ptr, src_dim, src_dim.x, ptr(), dimensions_, pos, op);
}

void surface_s::alpha_blend(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, int src_pitch, gpu::vec2i_t pos)
{
    auto op = [](auto& src, auto& dst) {
        dst *= src.a;
        dst /= 255;
        dst += src;
    };
    copy_operation(src_ptr, src_dim, src_dim.x, ptr(), dimensions_, pos, op);
}

void surface_s::alpha_blend(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, int src_pitch, gpu::vec2i_t pos)
{
    auto op = [](auto& src, auto& dst) {
        glm::ivec4 tmp(dst);
        tmp -= src;
        tmp = glm::max(tmp, glm::ivec4{});
        dst = tmp;
        dst += src;
    };
    copy_operation(src_ptr, src_dim, src_dim.x, ptr(), dimensions_, pos, op);
}

} // namespace miximus::render
