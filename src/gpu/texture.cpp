#include "texture.hpp"

namespace miximus::gpu {

texture_s::texture_s(vec2i_t dimensions, color_type_e color)
    : display_dimensions_(dimensions)
    , color_type_(color)
{
    texture_dimensions_ = dimensions;
    GLenum  internal_format;
    GLsizei mip_map_levels = 5;

    glCreateTextures(GL_TEXTURE_2D, 1, &id_);

    glTextureParameteri(id_, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTextureParameteri(id_, GL_TEXTURE_WRAP_T, GL_REPEAT);

    switch (color_type_) {
        case color_type_e::RGB:
            internal_format = GL_RGB8;
            format_         = GL_RGB;
            type_           = GL_UNSIGNED_BYTE;
            glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
            glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            break;
        case color_type_e::RGBA:
            internal_format = GL_RGBA8;
            format_         = GL_RGBA;
            type_           = GL_UNSIGNED_BYTE;
            glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
            glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            break;
        case color_type_e::BGRA:
            internal_format = GL_RGBA8;
            format_         = GL_BGRA;
            type_           = GL_UNSIGNED_BYTE;
            glTextureParameteri(id_, GL_TEXTURE_MIN_FILTER, GL_NEAREST_MIPMAP_LINEAR);
            glTextureParameteri(id_, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            break;
        case color_type_e::UYVY:
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

void texture_s::bind(GLuint sampler) { glBindTextureUnit(sampler, id_); }

void texture_s::unbind(GLuint sampler) { glBindTextureUnit(sampler, 0); }

void texture_s::generate_mip_maps() { glGenerateTextureMipmap(id_); }

} // namespace miximus::gpu
