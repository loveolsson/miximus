#include "fallback.hpp"
#include "gpu/texture.hpp"

namespace miximus::gpu::transfer::detail {

fallback_transfer_s::fallback_transfer_s(size_t size, direction_e dir)
    : transfer_i(size, dir)
{
}

fallback_transfer_s::~fallback_transfer_s() {}

bool fallback_transfer_s::perform_transfer(texture_s& texture)
{
    auto id   = texture.id();
    auto dims = texture.texture_dimensions();

    if (direction_ == direction_e::cpu_to_gpu) {
        glBindTexture(GL_TEXTURE_2D, id);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, dims.x, dims.y, texture.format(), texture.type(), ptr_);
        return true;
    } else {
        return true;
    }
}

} // namespace miximus::gpu::transfer::detail