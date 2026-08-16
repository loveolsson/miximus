#include "draw_state.hpp"

namespace miximus::gpu {

draw_state_s::draw_state_s()
{
    vertex_array_.bind();
    vertex_buffer_.bind();
}

void draw_state_s::draw()
{
    vertex_array_.bind();

    if (!blending_enabled_) {
        glDisablei(GL_BLEND, 0);
    }

    if (shader_ != nullptr) {
        shader_->use();
    }

    vertex_buffer_.draw();

    shader_program_s::unuse();
    if (!blending_enabled_) {
        glEnablei(GL_BLEND, 0);
    }
}

} // namespace miximus::gpu
