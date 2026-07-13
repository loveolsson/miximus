#include "surface.hpp"

#include "gpu/texture.hpp"

#include <algorithm>
#include <cstdint>
#include <glm/glm.hpp>
#include <memory>
#include <stdexcept>

namespace miximus::render {

surface_s::surface_s(gpu::vec2i_t dim)
    : dimensions_(dim)
{
    using transfer_i  = gpu::transfer::transfer_i;
    const size_t size = sizeof(rgba_pixel_t) * dim.x * dim.y;

    texture_  = std::make_unique<gpu::texture_s>(dim, gpu::texture_s::format_e::rgba_f16);
    transfer_ = transfer_i::create_transfer(transfer_i::get_prefered_type(), size, transfer_i::direction_e::cpu_to_gpu);
    gpu::transfer::transfer_i::register_texture(transfer_->type(), texture_.get());
}

surface_s::~surface_s() { gpu::transfer::transfer_i::unregister_texture(transfer_->type(), texture_.get()); }

void surface_s::clear(const rgba_pixel_t& color)
{
    std::fill(ptr(), ptr() + static_cast<size_t>(dimensions_.x * dimensions_.y), color);
}

namespace {
template <typename SrcT, typename Op>
void copy_operation(const SrcT*              src_ptr,
                    gpu::vec2i_t             src_dim,
                    size_t                   src_pitch,
                    surface_s::rgba_pixel_t* dst_ptr,
                    gpu::vec2i_t             dst_dim,
                    gpu::vec2i_t             pos,
                    Op                       op)
{
    if (src_dim.x < 0 || src_dim.y < 0 || dst_dim.x < 0 || dst_dim.y < 0) {
        throw std::length_error("copy_operation called with invalid dimensions");
    }
    if (src_pitch < static_cast<size_t>(src_dim.x) * sizeof(SrcT)) {
        throw std::length_error("copy_operation called with invalid pitch");
    }

    const auto src_x = std::max<int64_t>(0, -static_cast<int64_t>(pos.x));
    const auto src_y = std::max<int64_t>(0, -static_cast<int64_t>(pos.y));
    const auto dst_x = std::max<int64_t>(0, pos.x);
    const auto dst_y = std::max<int64_t>(0, pos.y);

    const auto width  = std::min(static_cast<int64_t>(src_dim.x) - src_x, static_cast<int64_t>(dst_dim.x) - dst_x);
    const auto height = std::min(static_cast<int64_t>(src_dim.y) - src_y, static_cast<int64_t>(dst_dim.y) - dst_y);

    if (width <= 0 || height <= 0) {
        return;
    }

    const auto* src_row = reinterpret_cast<const char*>(src_ptr) + (src_y * src_pitch);
    auto*       dst_row = dst_ptr + (dst_y * dst_dim.x) + dst_x;

    for (int64_t y = 0; y < height; ++y) {
        const auto* typed_src_row = reinterpret_cast<const SrcT*>(src_row) + src_x;

        for (int64_t x = 0; x < width; ++x) {
            op(typed_src_row[x], &dst_row[x]);
        }

        src_row += src_pitch;
        dst_row += dst_dim.x;
    }
}
} // namespace

void surface_s::copy(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    auto op = [](const auto& src, auto dst) { *dst = src; };
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, op);
}

void surface_s::copy(const mono_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    auto op = [](const auto& src, auto dst) { *dst = {src, src, src, src}; };
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, op);
}

void surface_s::alpha_blend(const rgba_pixel_t* src_ptr, gpu::vec2i_t src_dim, size_t src_pitch, gpu::vec2i_t pos)
{
    auto op = [](const auto& src, auto dst) {
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
    auto op = [](const auto& src, auto dst) {
        glm::ivec4 tmp(*dst);
        tmp -= src;
        tmp  = glm::max(tmp, glm::ivec4{});
        *dst = tmp;
        *dst += src;
    };
    copy_operation(src_ptr, src_dim, src_pitch, ptr(), dimensions_, pos, op);
}

} // namespace miximus::render
