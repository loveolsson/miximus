#pragma once
#include "gpu/glad.hpp"
#include "gpu/vertex.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace miximus::gpu {

class shader_program_s
{
    struct attribute_s
    {
        std::string name;
        GLint       loc;
        GLenum      type;
        GLint       size;
    };

    struct uniform_s
    {
        GLint  loc;
        GLenum type;
        GLint  size;
    };

    using attr_list_t   = std::vector<attribute_s>;
    using uniform_map_t = std::unordered_map<std::string, uniform_s>;

    GLuint        program_;
    attr_list_t   attributes_;
    uniform_map_t uniforms_;

  public:
    enum name_e
    {
        basic,
        yuv_to_rgb,
        apply_gamma,
    };

    shader_program_s(std::string_view vert_name, std::string_view frag_name);
    ~shader_program_s();

    shader_program_s(const shader_program_s&) = delete;
    shader_program_s(shader_program_s&&)      = delete;
    void operator=(const shader_program_s&) = delete;
    void operator=(shader_program_s&&) = delete;

    void        use() const;
    static void unuse();
    GLuint      get_id() { return program_; }

    template <typename T>
    void set_vertex_type();

    void set_uniform(const std::string& name, const vec2_t& val);
    void set_uniform(const std::string& name, const mat3& val);
    void set_uniform(const std::string& name, double val);
};

template <typename T>
inline void shader_program_s::set_vertex_type()
{
    constexpr auto info = get_vertex_type_info<T>();

    for (GLuint i = 0; i < attributes_.size(); ++i) {
        const auto& attr = attributes_[i];

        auto it = info.find(attr.name);
        if (it != info.end()) {
            const vertex_attr& v = it->second;

            glEnableVertexAttribArray(attr.loc);
            glVertexAttribPointer(attr.loc, v.size, v.type, v.norm, sizeof(T), reinterpret_cast<void*>(v.offset));
        } else {
            glDisableVertexAttribArray(attr.loc);
        }
    }
}

} // namespace miximus::gpu
