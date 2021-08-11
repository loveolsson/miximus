#pragma once
#include "glad.hpp"
#include "types.hpp"

namespace miximus::gpu {

constexpr GLuint MIP_MAP_LEVELS = 4;

class texture_s
{
  public:
    enum class format_e
    {
        rgb_f16,
        rgba_f16,
        bgra_u8,
        uyuv_u8,
        uyuv_u10,
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

    texture_s(const texture_s&) = delete;
    texture_s(texture_s&&)      = delete;
    void operator=(const texture_s&) = delete;
    void operator=(texture_s&&) = delete;

    void     init();
    vec2i_t  display_dimensions() { return display_dimensions_; }
    vec2i_t  texture_dimensions() { return texture_dimensions_; }
    GLenum   format() { return format_; }
    GLenum   type() { return type_; }
    format_e color_type() { return colorspace_; }
    GLuint   id() { return id_; }

    void        bind(GLuint sampler) const;
    static void unbind(GLuint sampler);
    void        generate_mip_maps() const;
};

} // namespace miximus::gpu
