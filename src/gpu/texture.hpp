#pragma once
#include "glad.hpp"
#include "types.hpp"

#include <cstddef>

namespace miximus::gpu {

constexpr GLuint MIP_MAP_LEVELS = 4;

class texture_s
{
  public:
    enum class pixel_format_e
    {
        rgb_f16,
        rgba_f16,
        rgba_u8,
        bgra_u8,
        uyuv_u8,
        uyuv_u10,
    };

    struct pixel_format_info_s
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
    GLuint         id_{};
    vec2i_t        display_dimensions_{};
    vec2i_t        texture_dimensions_{};
    GLenum         gl_external_format_{};
    GLenum         gl_external_type_{};
    pixel_format_e pixel_format_;

  public:
    texture_s(vec2i_t dimensions, pixel_format_e pixel_format);
    ~texture_s();

    texture_s(const texture_s&)      = delete;
    texture_s(texture_s&&)           = delete;
    void operator=(const texture_s&) = delete;
    void operator=(texture_s&&)      = delete;

    void                       init();
    static pixel_format_info_s pixel_format_info(pixel_format_e pixel_format);
    static size_t              host_row_byte_size(vec2i_t dimensions, pixel_format_e pixel_format);
    static size_t              estimate_storage_byte_size(vec2i_t dimensions, pixel_format_e pixel_format);
    vec2i_t                    display_dimensions() const noexcept { return display_dimensions_; }
    vec2i_t                    texture_dimensions() const noexcept { return texture_dimensions_; }
    GLenum                     gl_external_format() const noexcept { return gl_external_format_; }
    GLenum                     gl_external_type() const noexcept { return gl_external_type_; }
    pixel_format_e             pixel_format() const noexcept { return pixel_format_; }
    GLuint                     id() const noexcept { return id_; }

    void        bind(GLuint sampler) const;
    static void unbind(GLuint sampler);
    void        clear() const;
    void        generate_mip_maps() const;
};

} // namespace miximus::gpu
