#include "gpu/shader.hpp"
#include "logger/logger.hpp"
#include "static_files/files.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace miximus::gpu {

class shader_s
{
    GLuint id_{0};

  public:
    shader_s(std::string_view name, GLenum type)
        : id_(glCreateShader(type))
    {
        const auto& files       = static_files::get_resource_files();
        const auto  common_text = files.get_file_or_throw("shaders/common.glsl").unzip();
        const auto  shader_text = files.get_file_or_throw(name).unzip();

        const auto texts = std::array{
            "#version 330 core\n",
            common_text.c_str(),
            shader_text.c_str(),
        };

        glShaderSource(id_, static_cast<GLsizei>(texts.size()), texts.data(), nullptr);
        glCompileShader(id_);

        GLint is_compiled = 0;
        glGetShaderiv(id_, GL_COMPILE_STATUS, &is_compiled);

        if (is_compiled == GL_FALSE) {
            GLint length = 0;
            glGetShaderiv(id_, GL_INFO_LOG_LENGTH, &length);

            std::vector<GLchar> text(length);
            glGetShaderInfoLog(id_, length, nullptr, text.data());

            glDeleteShader(id_);

            throw std::runtime_error(text.data());
        }
    }

    ~shader_s() { glDeleteShader(id_); }

    shader_s(const shader_s&)       = delete;
    shader_s(shader_s&&)            = delete;
    void operator=(const shader_s&) = delete;
    void operator=(shader_s&&)      = delete;

    GLuint id() const { return id_; }
};

shader_program_s::shader_program_s(std::string_view vert_name, std::string_view frag_name)
    : program_(glCreateProgram())
{
    auto log = getlog("gpu");
    log->debug(R"(Compiling shader "{}"/"{}")", vert_name, frag_name);

    const shader_s vert(vert_name, GL_VERTEX_SHADER);
    const shader_s frag(frag_name, GL_FRAGMENT_SHADER);

    glAttachShader(program_, vert.id());
    glAttachShader(program_, frag.id());

    glLinkProgram(program_);

    GLint is_linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &is_linked);

    if (is_linked == GL_FALSE) {
        GLint length = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &length);

        std::vector<GLchar> text(length);
        glGetProgramInfoLog(program_, length, nullptr, text.data());
        glDeleteProgram(program_);

        throw std::runtime_error(text.data());
    }

    GLint count = 0;
    glGetProgramiv(program_, GL_ACTIVE_ATTRIBUTES, &count);
    log->debug("Active Attributes: {}", count);

    for (GLuint i = 0; i < count; i++) {
        std::array<GLchar, 32> name{};
        GLint                  size{};
        GLenum                 type{};

        glGetActiveAttrib(program_, i, name.size(), nullptr, &size, &type, name.data());

        log->debug(" -- Attribute {} Type: {} Name: \"{}\"", i, type, name.data());

        const GLint loc = glGetAttribLocation(program_, name.data());
        if (loc != -1) {
            attributes_.emplace_back(attribute_s{
                .name = name.data(),
                .loc  = loc,
                .type = type,
                .size = size,
            });
        }
    }

    glGetProgramiv(program_, GL_ACTIVE_UNIFORMS, &count);
    log->debug("Active Uniforms: {}", count);

    for (GLuint i = 0; i < count; i++) {
        std::array<GLchar, 32> name{};
        GLint                  size{};
        GLenum                 type{};

        glGetActiveUniform(program_, i, name.size(), nullptr, &size, &type, name.data());

        log->debug(" -- Uniform {} Type: {} Name: \"{}\"", i, type, name.data());

        const GLint loc = glGetUniformLocation(program_, name.data());
        if (loc != -1) {
            uniforms_.emplace(name.data(),
                              uniform_s{
                                  .loc  = loc,
                                  .type = type,
                                  .size = size,
                              });
        }
    }
}

shader_program_s::~shader_program_s() { glDeleteProgram(program_); }

void shader_program_s::use() const { glUseProgram(program_); }

void shader_program_s::unuse() { glUseProgram(0); }

void shader_program_s::set_uniform(const std::string& name, const vec2_t& val)
{
    if (auto it = uniforms_.find(name); it != uniforms_.end()) {
        glProgramUniform2f(program_, it->second.loc, static_cast<float>(val.x), static_cast<float>(val.y));
    }
}

void shader_program_s::set_uniform(const std::string& name, const mat3& val)
{
    if (auto it = uniforms_.find(name); it != uniforms_.end()) {
        glProgramUniformMatrix3fv(program_, it->second.loc, 1, GL_TRUE, &val[0][0]);
    }
}

void shader_program_s::set_uniform(const std::string& name, double val)
{
    if (auto it = uniforms_.find(name); it != uniforms_.end()) {
        glProgramUniform1f(program_, it->second.loc, static_cast<float>(val));
    }
}

void shader_program_s::set_uniform(const std::string& name, int val)
{
    if (auto it = uniforms_.find(name); it != uniforms_.end()) {
        glProgramUniform1i(program_, it->second.loc, val);
    }
}

} // namespace miximus::gpu
