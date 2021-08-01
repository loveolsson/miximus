#pragma once
#include "shader.hpp"
#include "vertex_array.hpp"
#include "vertex_buffer.hpp"

namespace miximus::gpu {
class draw_state_s
{
    vertex_array_s    vertex_array_;
    vertex_buffer_s   vertex_buffer_;
    shader_program_s* shader_;

  public:
    draw_state_s();
    ~draw_state_s();

    template <typename T, size_t N>
    void set_vertex_data(const T (&arr)[N])
    {
        vertex_buffer_.set_data(arr);
        if (shader_ != nullptr) {
            shader_->set_vertex_type<T>();
        }
    }

    void  set_shader_program(shader_program_s* shader) { shader_ = shader; }
    auto* get_shader_program() { return shader_; }

    void draw();
};
} // namespace miximus::gpu
