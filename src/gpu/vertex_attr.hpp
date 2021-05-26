#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>

namespace miximus::gpu {

struct vertex_attr
{
    GLint     size;
    GLenum    type;
    GLboolean norm;
    GLsizei   offset;
    GLenum    base_type;
};

} // namespace miximus::gpu
