#include "surface.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <stdexcept>

namespace miximus::render {

surface_s::surface_s(gpu::vec2i_t dim)
    : dimensions_(dim)
{
    using transfer_i  = gpu::transfer::transfer_i;
    const size_t size = sizeof(rgba_pixel_t) * dim.x * dim.y;

    texture_  = std::make_unique<gpu::texture_s>(dim, gpu::texture_s::format_e::rgba_f16);
    transfer_ = transfer_i::create_transfer(transfer_i::get_prefered_type(), size, transfer_i::direction_e::cpu_to_gpu);
}

void surface_s::clear(const rgba_pixel_t& color)
{
    std::fill(ptr(), ptr() + static_cast<size_t>(dimensions_.x * dimensions_.y), color);
}

template <typename SrcT, typename Op>
static inline void copy_operation(const SrcT*              src_ptr,
                                  gpu::vec2i_t             src_dim,
                                  size_t                   src_pitch,
                                  surface_s::rgba_pixel_t* dst_ptr,
                                  gpu::vec2i_t             dst_dim,
                                  gpu::vec2i_t             pos,
                                  Op                       op)
{
    if (src_pitch < src_dim.x * sizeof(SrcT)) {
        throw std::length_error("copy_operation called with invalid pitch");
    }

    const char* src_p = reinterpret_cast<const char*>(src_ptr);

    for (int sy = 0; sy < src_dim.y; ++sy) {
        const int dy = pos.y + sy;

        if (dy < 0) {
            continue;
        }

        if (dy >= dst_dim.y) {
            break;
        }

        for (int sx = 0; sx < src_dim.x; ++sx) {
            const int dx = pos.x + sx;
            if (dx < 0) {
                continue;
            }

            if (dx >= dst_dim.x) {
                break;
            }

            const auto& sp = reinterpret_cast<const SrcT*>(src_p)[sx];
            auto*       dp = &dst_ptr[dst_dim.x * dy + dx];

            op(sp, dp);
        }

        src_p += src_pitch;
    }
}

void surface_s::copy(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    auto op = [](const auto& src, auto* dst) { *dst = src; };
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, op);
}

void surface_s::copy(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    auto op = [](const auto& src, auto* dst) { *dst = {src, src, src, src}; };
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, op);
}

void surface_s::alpha_blend(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    auto op = [](const auto& src, auto* dst) {
        glm::ivec4 tmp(*dst);
        tmp -= src.a;
        tmp  = glm::max(tmp, glm::ivec4{});
        *dst = tmp;
        *dst += src;
    };
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, op);
}

void surface_s::alpha_blend(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    auto op = [](const auto& src, auto* dst) {
        glm::ivec4 tmp(*dst);
        tmp -= src;
        tmp  = glm::max(tmp, glm::ivec4{});
        *dst = tmp;
        *dst += src;
    };
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, op);
}

} // namespace miximus::render
