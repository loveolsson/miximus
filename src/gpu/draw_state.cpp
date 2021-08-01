#include "draw_state.hpp"

namespace miximus::gpu {

draw_state_s::draw_state_s()
{
    vertex_array_.bind();
    vertex_buffer_.bind();
}

draw_state_s::~draw_state_s() {}

void draw_state_s::draw()
{
    vertex_array_.bind();

    if (shader_) {
        shader_->use();
    }

    vertex_buffer_.draw();
}

} // namespace miximus::gpu
