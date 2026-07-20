#pragma once
#include "glad.hpp"
#include "types.hpp"

#include <cstddef>

namespace miximus::gpu {

constexpr GLuint MIP_MAP_LEVELS = 4;

class texture_s
{
  public:
    enum class format_e
    {
        rgb_f16,
        rgba_f16,
        rgba_u8,
        bgra_u8,
        uyuv_u8,
        uyuv_u10,
    };

    struct format_info_s
    {
        GLenum  internal_format;
        GLenum  external_format;
        GLenum  external_type;
        GLint   min_filter;
        GLint   mag_filter;
        size_t  host_bytes_per_texel;
        size_t  storage_bytes_per_texel;
        int     display_pixels_per_texel;
        GLsizei mip_map_levels;
        bool    storage_identical;
    };

  private:
    GLuint   id_{};
    vec2i_t  display_dimensions_{};
    vec2i_t  texture_dimensions_{};
    GLenum   format_{};
    GLenum   type_{};
    format_e colorspace_;

  public:
    texture_s(vec2i_t dimensions, format_e color);
    ~texture_s();

    texture_s(const texture_s&)      = delete;
    texture_s(texture_s&&)           = delete;
    void operator=(const texture_s&) = delete;
    void operator=(texture_s&&)      = delete;

    void                 init();
    static format_info_s format_info(format_e format);
    static size_t        host_row_byte_size(vec2i_t dimensions, format_e format);
    static size_t        estimate_storage_byte_size(vec2i_t dimensions, format_e format);
    vec2i_t              display_dimensions() { return display_dimensions_; }
    vec2i_t              texture_dimensions() { return texture_dimensions_; }
    GLenum               format() { return format_; }
    GLenum               type() { return type_; }
    format_e             color_type() { return colorspace_; }
    GLuint               id() { return id_; }

    void        bind(GLuint sampler) const;
    static void unbind(GLuint sampler);
    void        generate_mip_maps() const;
};

} // namespace miximus::gpu
