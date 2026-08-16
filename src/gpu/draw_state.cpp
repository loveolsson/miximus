#include "draw_state.hpp"

namespace miximus::gpu {

namespace {
constexpr GLuint position_attribute_location           = 0;
constexpr GLuint texture_coordinate_attribute_location = 1;
} // namespace

void draw_state_s::set_vertex_data(std::span<const vertex_uv> vertices)
{
    vertex_array_.bind();
    vertex_buffer_.bind();
    vertex_buffer_.set_data(vertices);

    // OpenGL represents an offset into the bound vertex buffer through this pointer parameter.
    glEnableVertexAttribArray(position_attribute_location);
    glVertexAttribPointer(position_attribute_location,
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(vertex_uv),
                          reinterpret_cast<void*>(offsetof(vertex_uv, pos))); // NOLINT(performance-no-int-to-ptr)

    glEnableVertexAttribArray(texture_coordinate_attribute_location);
    glVertexAttribPointer(texture_coordinate_attribute_location,
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(vertex_uv),
                          reinterpret_cast<void*>(offsetof(vertex_uv, uv))); // NOLINT(performance-no-int-to-ptr)
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
