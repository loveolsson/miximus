#pragma once
#include "glad.hpp"

#include <cstddef>

namespace miximus::gpu {

class vertex_buffer_s
{
  private:
    size_t vertex_count_{};
    GLuint id_{};

  public:
    vertex_buffer_s();
    ~vertex_buffer_s();

    void set_data(const void* data, size_t element_size, size_t count);

    template <typename T, size_t N>
    void set_data(T (&arr)[N])
    {
        set_data(arr, sizeof(T), N);
    }

    void bind() const;
    void draw() const;
};

} // namespace miximus::gpu