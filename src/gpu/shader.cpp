#include "gpu/shader.hpp"
#include "logger/logger.hpp"
#include "static_files/files.hpp"

#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace miximus::gpu {

constexpr size_t MAX_NAME_LEN = 16;

class shader_s
{
    GLuint id_;

  public:
    shader_s(std::string_view name, GLenum type)
        : id_(0)
    {
        const auto& files = static_files::get_resource_files();

        auto it = files.find(name);

        if (it == files.end()) {
            throw std::runtime_error("shader file not found: " + std::string(name));
        }

        id_ = glCreateShader(type);

        auto        shader_text     = it->second.raw();
        const auto* shader_text_ptr = shader_text.c_str();
        glShaderSource(id_, 1, &shader_text_ptr, nullptr);
        glCompileShader(id_);

        GLint is_compiled = 0;
        glGetShaderiv(id_, GL_COMPILE_STATUS, &is_compiled);

        if (is_compiled == GL_FALSE) {
            GLint max_length = 0;
            glGetShaderiv(id_, GL_INFO_LOG_LENGTH, &max_length);

            std::vector<GLchar> log(max_length);
            glGetShaderInfoLog(id_, max_length, &max_length, log.data());

            glDeleteShader(id_);

            throw std::runtime_error(std::string(log.data()));
        }
    }

    ~shader_s()
    {
        if (id_ != 0) {
            glDeleteShader(id_);
        }
    }

    shader_s(const shader_s&) = delete;
    shader_s(shader_s&&)      = delete;
    void operator=(const shader_s&) = delete;
    void operator=(shader_s&&) = delete;

    GLuint id() const { return id_; }
};

shader_program_s::shader_program_s(std::string_view vert_name, std::string_view frag_name)
    : program_(0)
{
    auto log = getlog("gpu");
    log->debug(R"(Compiling shader "{}"/"{}")", vert_name, frag_name);

    shader_s vert(vert_name, GL_VERTEX_SHADER);
    shader_s frag(frag_name, GL_FRAGMENT_SHADER);

    program_ = glCreateProgram();
    glAttachShader(program_, vert.id());
    glAttachShader(program_, frag.id());

    glLinkProgram(program_);

    GLint is_linked = 0;
    glGetProgramiv(program_, GL_LINK_STATUS, &is_linked);

    if (is_linked == GL_FALSE) {
        GLint max_length = 0;
        glGetProgramiv(program_, GL_INFO_LOG_LENGTH, &max_length);

        std::vector<GLchar> log(max_length);
        glGetProgramInfoLog(program_, max_length, &max_length, log.data());
        glDeleteProgram(program_);

        throw std::runtime_error(std::string(log.data()));
    }

    GLint                            count       = 0;
    GLint                            size        = 0;
    GLenum                           type        = 0;
    GLsizei                          name_length = 0;
    std::array<GLchar, MAX_NAME_LEN> name{};

    glGetProgramiv(program_, GL_ACTIVE_ATTRIBUTES, &count);
    log->debug("Active Attributes: {}", count);

    for (GLuint i = 0; i < count; i++) {
        glGetActiveAttrib(program_, i, MAX_NAME_LEN, &name_length, &size, &type, name.data());

        log->debug(" -- Attribute {} Type: {} Name: \"{}\"", i, type, name.data());

        GLint loc = glGetAttribLocation(program_, name.data());
        if (loc != -1) {
            attribute_s attr{};
            attr.name = name.data();
            attr.loc  = loc;
            attr.type = type;
            attr.size = size;

            attributes_.emplace_back(attr);
        }
    }

    glGetProgramiv(program_, GL_ACTIVE_UNIFORMS, &count);
    log->debug("Active Uniforms: {}", count);

    for (GLuint i = 0; i < count; i++) {
        glGetActiveUniform(program_, i, MAX_NAME_LEN, &name_length, &size, &type, name.data());

        log->debug(" -- Uniform {} Type: {} Name: \"{}\"", i, type, name.data());

        GLint loc = glGetUniformLocation(program_, name.data());
        if (loc != -1) {
            uniform_s uni{};
            uni.loc  = loc;
            uni.type = type;
            uni.size = size;

            uniforms_.emplace(name.data(), uni);
        }
    }
}

shader_program_s::~shader_program_s()
{
    if (program_ != 0) {
        glDeleteProgram(program_);
    }
}

void shader_program_s::use() const { glUseProgram(program_); }

void shader_program_s::unuse() { glUseProgram(0); }

void shader_program_s::set_uniform(const std::string& name, const vec2_t& val)
{
    if (auto it = uniforms_.find(name); it != uniforms_.end()) {
        glProgramUniform2f(program_, it->second.loc, val.x, val.y);
    }
}

} // namespace miximus::gpu
