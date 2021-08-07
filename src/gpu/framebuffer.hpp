#pragma once
#include "texture.hpp"

#include <memory>

namespace miximus::gpu {

class framebuffer_s
{
    GLuint                     id_{};
    GLuint                     rbo_id_{};
    std::unique_ptr<texture_s> texture_;

  public:
    framebuffer_s(vec2i_t dimensions, texture_s::colorspace_e color);
    ~framebuffer_s();

    void        bind() const;
    static void unbind();
    texture_s*  texture() { return texture_.get(); }
};

} // namespace miximus::gpu