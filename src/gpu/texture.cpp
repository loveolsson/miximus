#include "texture.hpp"

#include "context.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace miximus::gpu {
namespace {
size_t checked_add(size_t lhs, size_t rhs)
{
    if (rhs > std::numeric_limits<size_t>::max() - lhs) {
        throw std::overflow_error("texture storage byte size overflow");
    }
    return lhs + rhs;
}

size_t checked_multiply(size_t lhs, size_t rhs)
{
    if (lhs != 0 && rhs > std::numeric_limits<size_t>::max() / lhs) {
        throw std::overflow_error("texture storage byte size overflow");
    }
    return lhs * rhs;
}
} // namespace

texture_s::format_info_s texture_s::format_info(format_e format)
{
    switch (format) {
        case format_e::rgb_f16:
            return {
                .internal_format          = GL_RGB16,
                .external_format          = GL_RGB,
                .external_type            = GL_UNSIGNED_BYTE,
                .min_filter               = GL_NEAREST_MIPMAP_LINEAR,
                .mag_filter               = GL_LINEAR,
                .host_bytes_per_texel     = 3,
                .storage_bytes_per_texel  = 6,
                .display_pixels_per_texel = 1,
                .mip_map_levels           = static_cast<GLsizei>(MIP_MAP_LEVELS),
                .storage_identical        = false,
            };
        case format_e::rgba_f16:
            return {
                .internal_format          = GL_RGBA16,
                .external_format          = GL_RGBA,
                .external_type            = GL_UNSIGNED_BYTE,
                .min_filter               = GL_NEAREST_MIPMAP_LINEAR,
                .mag_filter               = GL_LINEAR,
                .host_bytes_per_texel     = 4,
                .storage_bytes_per_texel  = 8,
                .display_pixels_per_texel = 1,
                .mip_map_levels           = static_cast<GLsizei>(MIP_MAP_LEVELS),
                .storage_identical        = false,
            };
        case format_e::rgba_u8:
            return {
                .internal_format          = GL_RGBA8,
                .external_format          = GL_RGBA,
                .external_type            = GL_UNSIGNED_BYTE,
                .min_filter               = GL_NEAREST_MIPMAP_LINEAR,
                .mag_filter               = GL_LINEAR,
                .host_bytes_per_texel     = 4,
                .storage_bytes_per_texel  = 4,
                .display_pixels_per_texel = 1,
                .mip_map_levels           = static_cast<GLsizei>(MIP_MAP_LEVELS),
                .storage_identical        = true,
            };
        case format_e::bgra_u8:
            return {
                .internal_format          = GL_RGBA8,
                .external_format          = GL_BGRA,
                .external_type            = GL_UNSIGNED_INT_8_8_8_8_REV,
                .min_filter               = GL_NEAREST_MIPMAP_LINEAR,
                .mag_filter               = GL_LINEAR,
                .host_bytes_per_texel     = 4,
                .storage_bytes_per_texel  = 4,
                .display_pixels_per_texel = 1,
                .mip_map_levels           = static_cast<GLsizei>(MIP_MAP_LEVELS),
                .storage_identical        = false,
            };
        case format_e::uyuv_u8:
            return {
                .internal_format          = GL_RGBA8,
                .external_format          = GL_BGRA,
                .external_type            = GL_UNSIGNED_INT_8_8_8_8_REV,
                .min_filter               = GL_NEAREST,
                .mag_filter               = GL_NEAREST,
                .host_bytes_per_texel     = 4,
                .storage_bytes_per_texel  = 4,
                .display_pixels_per_texel = 2,
                .mip_map_levels           = 1,
                .storage_identical        = false,
            };
        case format_e::uyuv_u10:
            return {
                .internal_format          = GL_RGB10_A2,
                .external_format          = GL_RGBA,
                .external_type            = GL_UNSIGNED_INT_2_10_10_10_REV,
                .min_filter               = GL_NEAREST,
                .mag_filter               = GL_NEAREST,
                .host_bytes_per_texel     = 4,
                .storage_bytes_per_texel  = 4,
                .display_pixels_per_texel = 1,
                .mip_map_levels           = 1,
                .storage_identical        = true,
            };
    }
    throw std::invalid_argument("Invalid texture format");
}

texture_s::texture_s(vec2i_t dimensions, format_e color)
    : display_dimensions_(dimensions)
    , texture_dimensions_(dimensions)
    , colorspace_(color)
{
    const auto info = format_info(color);
    texture_dimensions_.x /= info.display_pixels_per_texel;
    format_ = info.external_format;
    type_   = info.external_type;

    glCreateTextures(GL_TEXTURE_2D, 1, &id_);

    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, info.min_filter);
    glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, info.mag_filter);

    glTextureStorage2D(id_, info.mip_map_levels, info.internal_format, texture_dimensions_.x, texture_dimensions_.y);
}

texture_s::~texture_s()
{
    if (!context_s::require_current()) {
        return;
    }
    glDeleteTextures(1, &id_);
}

void texture_s::bind(GLuint sampler) const { glBindTextureUnit(sampler, id_); }

void texture_s::unbind(GLuint sampler) { glBindTextureUnit(sampler, 0); }

size_t texture_s::estimate_storage_byte_size(vec2i_t dimensions, format_e format)
{
    if (dimensions.x <= 0 || dimensions.y <= 0) {
        throw std::invalid_argument("texture dimensions must be positive");
    }

    const auto info   = format_info(format);
    auto       width  = static_cast<size_t>(dimensions.x / info.display_pixels_per_texel);
    auto       height = static_cast<size_t>(dimensions.y);
    size_t     byte_size{};

    for (GLsizei level = 0; level < info.mip_map_levels; ++level) {
        const auto texel_count = checked_multiply(width, height);
        byte_size              = checked_add(byte_size, checked_multiply(texel_count, info.storage_bytes_per_texel));
        width                  = std::max<size_t>(1, width / 2);
        height                 = std::max<size_t>(1, height / 2);
    }

    return byte_size;
}

size_t texture_s::host_row_byte_size(vec2i_t dimensions, format_e format)
{
    if (dimensions.x <= 0 || dimensions.y <= 0) {
        throw std::invalid_argument("texture dimensions must be positive");
    }

    const auto info = format_info(format);
    if (dimensions.x % info.display_pixels_per_texel != 0) {
        throw std::invalid_argument("texture width is incompatible with packed pixel format");
    }
    return checked_multiply(static_cast<size_t>(dimensions.x / info.display_pixels_per_texel),
                            info.host_bytes_per_texel);
}

void texture_s::generate_mip_maps() const
{
    if (format_info(colorspace_).mip_map_levels > 1) {
        glGenerateTextureMipmap(id_);
    }
}

} // namespace miximus::gpu
