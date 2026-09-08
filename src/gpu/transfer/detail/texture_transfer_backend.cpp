#include "texture_transfer_backend.hpp"

namespace miximus::gpu::transfer::detail {

texture_transfer_backend_i::texture_transfer_backend_i(size_t host_buffer_size_bytes, direction_e direction)
    : host_buffer_size_bytes_(host_buffer_size_bytes)
    , direction_(direction)
{
}

texture_transfer_backend_i::~texture_transfer_backend_i() = default;

bool texture_transfer_backend_i::register_texture(texture_s* texture)
{
    if (texture_ != nullptr || texture == nullptr || !register_texture_impl(texture)) {
        return false;
    }
    texture_ = texture;
    return true;
}

bool texture_transfer_backend_i::unregister_texture()
{
    if (texture_ == nullptr) {
        return true;
    }
    const bool success = unregister_texture_impl(texture_);
    texture_           = nullptr;
    return success;
}

bool texture_transfer_backend_i::acquire_texture_for_gl()
{
    return texture_ != nullptr && acquire_texture_for_gl_impl(texture_);
}

bool texture_transfer_backend_i::release_texture_from_gl()
{
    return texture_ != nullptr && release_texture_from_gl_impl(texture_);
}

} // namespace miximus::gpu::transfer::detail
