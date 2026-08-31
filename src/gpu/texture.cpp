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

texture_s::storage_format_info_s texture_s::storage_format_info(storage_format_e storage_format)
{
    switch (storage_format) {
        case storage_format_e::rgb_unorm16:
            return {
                .internal_format         = GL_RGB16,
                .clear_format            = GL_RGB,
                .clear_type              = GL_UNSIGNED_SHORT,
                .min_filter              = GL_NEAREST_MIPMAP_LINEAR,
                .mag_filter              = GL_LINEAR,
                .storage_bytes_per_texel = 6,
                .mip_map_levels          = static_cast<GLsizei>(MIP_MAP_LEVELS),
                .integer                 = false,
            };
        case storage_format_e::rgba_unorm16:
            return {
                .internal_format         = GL_RGBA16,
                .clear_format            = GL_RGBA,
                .clear_type              = GL_UNSIGNED_SHORT,
                .min_filter              = GL_NEAREST_MIPMAP_LINEAR,
                .mag_filter              = GL_LINEAR,
                .storage_bytes_per_texel = 8,
                .mip_map_levels          = static_cast<GLsizei>(MIP_MAP_LEVELS),
                .integer                 = false,
            };
        case storage_format_e::rgba_unorm8:
            return {
                .internal_format         = GL_RGBA8,
                .clear_format            = GL_RGBA,
                .clear_type              = GL_UNSIGNED_BYTE,
                .min_filter              = GL_NEAREST_MIPMAP_LINEAR,
                .mag_filter              = GL_LINEAR,
                .storage_bytes_per_texel = 4,
                .mip_map_levels          = static_cast<GLsizei>(MIP_MAP_LEVELS),
                .integer                 = false,
            };
        case storage_format_e::r32_uint:
            return {
                .internal_format         = GL_R32UI,
                .clear_format            = GL_RED_INTEGER,
                .clear_type              = GL_UNSIGNED_INT,
                .min_filter              = GL_NEAREST,
                .mag_filter              = GL_NEAREST,
                .storage_bytes_per_texel = 4,
                .mip_map_levels          = 1,
                .integer                 = true,
            };
    }
    throw std::invalid_argument("Invalid texture storage format");
}

texture_s::texture_s(vec2i_t                   display_dimensions,
                     vec2i_t                   texture_dimensions,
                     storage_format_e          storage_format,
                     input_component_mapping_e input_component_mapping)
    : display_dimensions_(display_dimensions)
    , texture_dimensions_(texture_dimensions)
    , storage_format_(storage_format)
    , input_component_mapping_(input_component_mapping)
{
    const auto info = storage_format_info(storage_format);

    glCreateTextures(GL_TEXTURE_2D, 1, &id_);

    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, info.min_filter);
    glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, info.mag_filter);

    glTextureStorage2D(id_, info.mip_map_levels, info.internal_format, texture_dimensions_.x, texture_dimensions_.y);
}

texture_s::texture_s(vec2i_t                   dimensions,
                     storage_format_e          storage_format,
                     input_component_mapping_e input_component_mapping)
    : texture_s(dimensions, dimensions, storage_format, input_component_mapping)
{
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

void texture_s::clear() const
{
    const auto info           = storage_format_info(storage_format_);
    const auto mip_map_levels = info.mip_map_levels;
    for (GLsizei level = 0; level < mip_map_levels; ++level) {
        glClearTexImage(id_, level, info.clear_format, info.clear_type, nullptr);
    }
}

size_t texture_s::estimate_storage_byte_size(vec2i_t dimensions, storage_format_e storage_format)
{
    if (dimensions.x <= 0 || dimensions.y <= 0) {
        throw std::invalid_argument("texture dimensions must be positive");
    }

    const auto info   = storage_format_info(storage_format);
    auto       width  = static_cast<size_t>(dimensions.x);
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

void texture_s::generate_mip_maps() const
{
    if (storage_format_info(storage_format_).mip_map_levels > 1) {
        glGenerateTextureMipmap(id_);
    }
}

} // namespace miximus::gpu
