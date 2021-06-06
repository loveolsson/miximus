#include "gpu/shader.hpp"
#include "logger/logger.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace miximus::gpu {

class shader
{
    GLuint id_;

  public:
    shader(const static_files::file_map_t& files, std::string_view name, GLenum type)
        : id_(0)
    {
        auto it = files.find(name);

        if (it == files.end()) {
            throw std::runtime_error("shader file not found: " + std::string(name));
        }

        id_ = glCreateShader(type);

        auto shader_text = it->second.raw.c_str();
        glShaderSource(id_, 1, &shader_text, NULL);
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

    ~shader()
    {
        if (id_ != 0) {
            glDeleteShader(id_);
        }
    }

    GLuint id() { return id_; }
};

shader_program::shader_program(const static_files::file_map_t& files,
                               std::string_view                vert_name,
                               std::string_view                frag_name)
    : program_(0)
{
    auto logger = spdlog::get("gpu");

    shader vert(files, vert_name, GL_VERTEX_SHADER);
    shader frag(files, frag_name, GL_FRAGMENT_SHADER);

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

    GLint   count;
    GLint   size;
    GLenum  type;
    GLchar  name[16];
    GLsizei name_length;

    glGetProgramiv(program_, GL_ACTIVE_ATTRIBUTES, &count);
    logger->info("Active Attributes: {}", count);

    for (GLuint i = 0; i < count; i++) {
        glGetActiveAttrib(program_, i, sizeof(name), &name_length, &size, &type, name);

        logger->info("Attribute {} Type: {} Name: {}", i, type, name);

        GLint loc = glGetAttribLocation(program_, name);
        if (loc != -1) {
            attribute attr;
            attr.name = name;
            attr.loc  = loc;
            attr.type = type;
            attr.size = size;

            attributes_.emplace_back(attr);
        }
    }

    glGetProgramiv(program_, GL_ACTIVE_UNIFORMS, &count);
    logger->info("Active Uniforms: {}", count);

    for (GLuint i = 0; i < count; i++) {
        glGetActiveUniform(program_, i, sizeof(name), &name_length, &size, &type, name);

        logger->info("Uniform {} Type: {} Name: {}", i, type, name);

        GLint loc = glGetUniformLocation(program_, name);
        if (loc != -1) {
            uniform uni;
            uni.loc  = loc;
            uni.type = type;
            uni.size = size;

            uniforms_.emplace(name, uni);
        }
    }
}

shader_program::shader_program(shader_program&& o) noexcept
{
    program_    = o.program_;
    o.program_  = 0;
    attributes_ = std::move(o.attributes_);
    uniforms_   = std::move(o.uniforms_);
}

shader_program::~shader_program()
{
    if (program_ != 0) {
        glUseProgram(0);
        glDeleteProgram(program_);
    }
}

shader_store::shader_store()
{
    auto files = static_files::get_shader_files();
    shaders_.emplace("basic", shader_program{files, "basic_vert.glsl", "basic_frag.glsl"});
}

shader_store::~shader_store() {}

shader_program& shader_store::get_shader(std::string_view name)
{
    auto it = shaders_.find(name);
    if (it == shaders_.end()) {
        throw std::runtime_error("shader not found");
    }

    return it->second;
}

} // namespace miximus::gpu
