#include "gpu/shader.hpp"

#include "gpu/context.hpp"
#include "logger/logger.hpp"
#include "static_files/files.hpp"

#include <array>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace miximus::gpu {

namespace {

void maybe_throw_uniform_type_error(bool             type_matches,
                                    std::string_view uniform_name,
                                    GLenum           actual_type,
                                    GLint            array_size,
                                    std::string_view expected_type)
{
#ifdef MIXIMUS_VALIDATE_SHADER_UNIFORMS
    if (!type_matches) {
        throw std::logic_error(std::format("shader uniform '{}' has OpenGL type {} and array size {}; expected {}",
                                           uniform_name,
                                           actual_type,
                                           array_size,
                                           expected_type));
    }
#else
    (void)type_matches;
    (void)uniform_name;
    (void)actual_type;
    (void)array_size;
    (void)expected_type;
#endif
}

} // namespace

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

            getlog("gpu")->error("Failed to compile shader {}: {}", name, text.data());

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
    glGetProgramiv(program_, GL_ACTIVE_UNIFORMS, &count);
    log->debug("Active Uniforms: {}", count);

    GLint uniform_name_length = 0;
    glGetProgramiv(program_, GL_ACTIVE_UNIFORM_MAX_LENGTH, &uniform_name_length);
    std::vector<GLchar> uniform_name(static_cast<size_t>(uniform_name_length));

    for (GLuint i = 0; std::cmp_less(i, count); i++) {
        GLint  size{};
        GLenum type{};

        glGetActiveUniform(program_, i, uniform_name_length, nullptr, &size, &type, uniform_name.data());

        log->debug(" -- Uniform {} Type: {} Name: \"{}\"", i, type, uniform_name.data());

        const GLint loc = glGetUniformLocation(program_, uniform_name.data());
        if (loc != -1) {
            uniforms_.emplace(uniform_name.data(),
                              uniform_s{
                                  .location = loc,
                                  .type     = type,
                                  .size     = size,
                              });
        }
    }
}

shader_program_s::~shader_program_s()
{
    if (!context_s::require_current()) {
        return;
    }
    glDeleteProgram(program_);
}

void shader_program_s::use() const { glUseProgram(program_); }

void shader_program_s::unuse() { glUseProgram(0); }

const shader_program_s::uniform_s* shader_program_s::find_uniform(std::string_view name) const noexcept
{
    const auto it = uniforms_.find(name);
    return it != uniforms_.end() ? &it->second : nullptr;
}

bool shader_program_s::set_uniform(std::string_view name, const vec2_t& val)
{
    if (const auto* uniform = find_uniform(name)) {
        maybe_throw_uniform_type_error(
            uniform->type == GL_FLOAT_VEC2, name, uniform->type, uniform->size, "GL_FLOAT_VEC2");
        glProgramUniform2f(program_, uniform->location, static_cast<float>(val.x), static_cast<float>(val.y));
        return true;
    }
    return false;
}

bool shader_program_s::set_uniform(std::string_view name, const vec3_t& val)
{
    if (const auto* uniform = find_uniform(name)) {
        maybe_throw_uniform_type_error(
            uniform->type == GL_FLOAT_VEC3, name, uniform->type, uniform->size, "GL_FLOAT_VEC3");
        glProgramUniform3f(program_, uniform->location, val.x, val.y, val.z);
        return true;
    }
    return false;
}

bool shader_program_s::set_uniform(std::string_view name, const mat3& val)
{
    if (const auto* uniform = find_uniform(name)) {
        maybe_throw_uniform_type_error(
            uniform->type == GL_FLOAT_MAT3, name, uniform->type, uniform->size, "GL_FLOAT_MAT3");
        glProgramUniformMatrix3fv(program_, uniform->location, 1, GL_TRUE, &val[0][0]);
        return true;
    }
    return false;
}

bool shader_program_s::set_uniform(std::string_view name, double val)
{
    if (const auto* uniform = find_uniform(name)) {
        maybe_throw_uniform_type_error(uniform->type == GL_FLOAT, name, uniform->type, uniform->size, "GL_FLOAT");
        glProgramUniform1f(program_, uniform->location, static_cast<float>(val));
        return true;
    }
    return false;
}

bool shader_program_s::set_uniform(std::string_view name, int val)
{
    if (const auto* uniform = find_uniform(name)) {
        maybe_throw_uniform_type_error(uniform->type == GL_INT || uniform->type == GL_BOOL ||
                                           uniform->type == GL_SAMPLER_2D,
                                       name,
                                       uniform->type,
                                       uniform->size,
                                       "GL_INT, GL_BOOL, or GL_SAMPLER_2D");
        glProgramUniform1i(program_, uniform->location, val);
        return true;
    }
    return false;
}

} // namespace miximus::gpu
