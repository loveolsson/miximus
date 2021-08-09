#include "texture.hpp"

#include <stdexcept>

namespace miximus::gpu {

texture_s::texture_s(vec2i_t dimensions, colorspace_e color)
    : display_dimensions_(dimensions)
    , colorspace_(color)
{
    texture_dimensions_ = dimensions;
    GLenum  internal_format{};
    GLsizei mip_map_levels = MIP_MAP_LEVELS;

    glCreateTextures(GL_TEXTURE_2D, 1, &id_);

    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, GL_REPEAT);

    switch (colorspace_) {
        case colorspace_e::RGB:
            internal_format = GL_RGB8;
            format_         = GL_RGB;
            type_           = GL_UNSIGNED_BYTE;
            glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
            glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            break;
        case colorspace_e::RGBA:
            internal_format = GL_RGBA8;
            format_         = GL_RGBA;
            type_           = GL_UNSIGNED_BYTE;
            glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
            glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            // glTexParameterf(id_, GL_TEXTURE_MAX_ANISOTROPY, 16.f);
            break;
        case colorspace_e::BGRA:
            internal_format = GL_RGBA8;
            format_         = GL_BGRA;
            type_           = GL_UNSIGNED_BYTE;
            glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
            glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            break;
        case colorspace_e::UYVY:
            internal_format = GL_RGBA8;
            format_         = GL_BGRA;
            type_           = GL_UNSIGNED_INT_8_8_8_8_REV;
            texture_dimensions_.x /= 2;
            glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            mip_map_levels = 1;
            break;

        default:
            throw std::runtime_error("Invalid texture type");
            break;
    }

    glTextureStorage2D(id_, mip_map_levels, internal_format, texture_dimensions_.x, texture_dimensions_.y);
}

texture_s::~texture_s() { glDeleteTextures(1, &id_); }

void texture_s::bind(GLuint sampler) const { glBindTextureUnit(sampler, id_); }

void texture_s::unbind(GLuint sampler) { glBindTextureUnit(sampler, 0); }

void texture_s::generate_mip_maps() const
{
    switch (colorspace_) {
        case colorspace_e::UYVY:
            break;

        default:
            glGenerateTextureMipmap(id_);
            break;
    }
}

} // namespace miximus::gpu
