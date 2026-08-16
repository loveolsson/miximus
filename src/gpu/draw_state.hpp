#pragma once
#include "shader.hpp"
#include "vertex_array.hpp"
#include "vertex_buffer.hpp"

#include <ranges>
#include <span>

namespace miximus::gpu {
class draw_state_s
{
    vertex_array_s    vertex_array_;
    vertex_buffer_s   vertex_buffer_;
    shader_program_s* shader_{};
    bool              blending_enabled_{true};

  public:
    draw_state_s();
    ~draw_state_s() = default;

    draw_state_s(const draw_state_s&)   = delete;
    draw_state_s(draw_state_s&&)        = delete;
    void operator=(const draw_state_s&) = delete;
    void operator=(draw_state_s&&)      = delete;

    template <std::ranges::contiguous_range Range>
        requires std::ranges::sized_range<Range>
    void set_vertex_data(const Range& vertices)
    {
        using vertex_t = std::ranges::range_value_t<Range>;
        vertex_buffer_.set_data(std::span{vertices});
        if (shader_ != nullptr) {
            shader_->set_vertex_type<vertex_t>();
        }
    }

    void set_shader_program(shader_program_s* shader) { shader_ = shader; }
    void set_blending_enabled(bool enabled) noexcept { blending_enabled_ = enabled; }

    void draw();
};
} // namespace miximus::gpu
