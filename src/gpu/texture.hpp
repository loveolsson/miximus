#pragma once
#include "component_mapping.hpp"
#include "glad.hpp"
#include "types.hpp"

#include <cstddef>

namespace miximus::gpu {

class texture_s
{
  public:
    enum class storage_format_e
    {
        rgb_unorm16,
        rgba_unorm16,
        rgba_unorm8,
        r32_uint,
    };

    enum class sampling_e
    {
        nearest,
        linear,
        mipmapped_linear,
    };

    struct storage_format_info_s
    {
        GLenum internal_format;
        GLenum clear_format;
        GLenum clear_type;
        size_t storage_bytes_per_texel;
        bool   integer;
    };

  private:
    GLuint                    id_{};
    vec2i_t                   display_dimensions_{};
    vec2i_t                   texture_dimensions_{};
    storage_format_e          storage_format_;
    sampling_e                sampling_;
    GLsizei                   mip_map_levels_{};
    input_component_mapping_e input_component_mapping_{input_component_mapping_e::identity};

  public:
    texture_s(vec2i_t                   display_dimensions,
              vec2i_t                   texture_dimensions,
              storage_format_e          storage_format,
              input_component_mapping_e input_component_mapping = input_component_mapping_e::identity,
              sampling_e                sampling                = sampling_e::mipmapped_linear);
    texture_s(vec2i_t                   dimensions,
              storage_format_e          storage_format,
              input_component_mapping_e input_component_mapping = input_component_mapping_e::identity,
              sampling_e                sampling                = sampling_e::mipmapped_linear);
    ~texture_s();

    texture_s(const texture_s&)      = delete;
    texture_s(texture_s&&)           = delete;
    void operator=(const texture_s&) = delete;
    void operator=(texture_s&&)      = delete;

    void                                       init();
    [[nodiscard]] static storage_format_info_s storage_format_info(storage_format_e storage_format);
    [[nodiscard]] static GLsizei
    mip_map_level_count(vec2i_t dimensions, storage_format_e storage_format, sampling_e sampling);
    [[nodiscard]] static size_t estimate_storage_byte_size(vec2i_t          texture_dimensions,
                                                           storage_format_e storage_format,
                                                           sampling_e       sampling = sampling_e::mipmapped_linear);
    vec2i_t                     display_dimensions() const noexcept { return display_dimensions_; }
    vec2i_t                     texture_dimensions() const noexcept { return texture_dimensions_; }
    storage_format_e            storage_format() const noexcept { return storage_format_; }
    sampling_e                  sampling() const noexcept { return sampling_; }
    GLsizei                     mip_map_levels() const noexcept { return mip_map_levels_; }
    input_component_mapping_e   input_component_mapping() const noexcept { return input_component_mapping_; }
    GLuint                      id() const noexcept { return id_; }

    void        bind(GLuint sampler) const;
    static void unbind(GLuint sampler);
    void        clear() const;
    void        generate_mip_maps() const;
};

} // namespace miximus::gpu
