#pragma once
#include "gpu/glad.hpp"

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
