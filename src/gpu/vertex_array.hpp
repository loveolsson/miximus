#pragma once
#include "glad.hpp"
#include <cstddef>

namespace miximus::gpu {

class vertex_array_s
{
  private:
    GLuint id_{};

  public:
    vertex_array_s();
    ~vertex_array_s();

    void bind();
    void unbind();
};

} // namespace miximus::gpu