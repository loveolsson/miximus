#pragma once
#include "gpu/glad.hpp"
#include "gpu/types.hpp"
#include "utils/transparent_string_hash.hpp"

#include <string>
#include <string_view>
#include <unordered_map>

namespace miximus::gpu {

class shader_program_s
{
    struct uniform_s
    {
        GLint  location;
        GLenum type;
        GLint  size;
    };

    using uniform_map_t = std::unordered_map<std::string, uniform_s, utils::transparent_string_hash, std::equal_to<>>;

    GLuint        program_;
    uniform_map_t uniforms_;

    const uniform_s* find_uniform(std::string_view name) const noexcept;

  public:
    enum name_e
    {
        basic,
        texture_mix,
        yuv_to_rgb,
        rgb_to_yuv,
        apply_gamma,
        encode_rec709_premultiplied,
        strip_gamma,
    };

    shader_program_s(std::string_view vert_name, std::string_view frag_name);
    ~shader_program_s();

    shader_program_s(const shader_program_s&) = delete;
    shader_program_s(shader_program_s&&)      = delete;
    void operator=(const shader_program_s&)   = delete;
    void operator=(shader_program_s&&)        = delete;

    void        use() const;
    static void unuse();
    GLuint      get_id() { return program_; }

    bool set_uniform(std::string_view name, const vec2_t& val);
    bool set_uniform(std::string_view name, const vec3_t& val);
    bool set_uniform(std::string_view name, const mat3& val);
    bool set_uniform(std::string_view name, double val);
    bool set_uniform(std::string_view name, int val);
};

} // namespace miximus::gpu
