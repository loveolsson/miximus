#pragma once
#include "gpu/vertex.hpp"

#include <glad/glad.h>

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace miximus::gpu {

class shader_program
{
    struct attr
    {
        std::string name;
        GLint       loc;
        GLenum      type;
        GLint       size;
    };

    struct uniform
    {
        GLint  loc;
        GLenum type;
        GLint  size;
    };

    typedef std::vector<attr>                        attr_list_t;
    typedef std::unordered_map<std::string, uniform> uniform_map_t;

    GLuint        program_;
    attr_list_t   attributes_;
    uniform_map_t uniforms_;

  public:
    shader_program(std::string_view vert_name, std::string_view frag_name);
    ~shader_program();

    shader_program(const shader_program&) = delete;
    shader_program(shader_program&&);

    void        use();
    static void use_none();

    template <typename T>
    void set_vertex_type()
    {
        constexpr auto info = get_vertex_type_info<T>();

        for (GLuint i = 0; i < attributes_.size(); ++i) {
            const auto& attr = attributes_[i];

            auto it = info.find(attr.name);
            if (it != info.end()) {
                const vertex_attr& v = it->second;

                if (v.type != attr.type) {
                    throw std::runtime_error("attribute type missmatch");
                }

                glEnableVertexAttribArray(attr.loc);
                glVertexAttribPointer(attr.loc, v.size, v.type, v.norm, sizeof(T), (void*)v.offset);
            } else {
                glDisableVertexAttribArray(attr.loc);
            }
        }
    }
};

class shader_store
{
    typedef std::unordered_map<std::string_view, shader_program> map_t;

    static map_t shaders_;

  public:
    static shader_program& get_shader(std::string_view name)
    {
        auto it = shaders_.find(name);
        if (it == shaders_.end()) {
            throw std::runtime_error("shader not found");
        }

        return it->second;
    }

    static void init();
    static void clear();
};

} // namespace miximus::gpu
