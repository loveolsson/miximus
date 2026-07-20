#pragma once
#include "glad.hpp"

#include <cstddef>
#include <span>

namespace miximus::gpu {

class vertex_buffer_s
{
  private:
    size_t vertex_count_{};
    GLuint id_{};

    void set_data_bytes(std::span<const std::byte> data, size_t element_count);

  public:
    vertex_buffer_s();
    ~vertex_buffer_s();

    template <typename T, size_t Extent>
    void set_data(std::span<const T, Extent> data)
    {
        set_data_bytes(std::as_bytes(data), data.size());
    }

    void bind() const;
    void draw() const;
};

} // namespace miximus::gpu
