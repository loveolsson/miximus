#pragma once
#include "gpu/sync.hpp"

#include <glad/glad.h>

#include <memory>

namespace miximus::gpu {

class texture
{
    GLuint texture_;
    GLint  width_;
    GLint  height_;
    GLenum format_;
    GLenum type_;

    std::unique_ptr<sync> sync_;

  public:
    texture();
    ~texture();

    texture(const texture&) = delete;
    texture(texture&&)      = delete;
};

} // namespace miximus::gpu
