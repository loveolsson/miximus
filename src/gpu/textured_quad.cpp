#include "textured_quad.hpp"

#include "texture.hpp"
#include "vertex.hpp"

#include <stdexcept>

namespace miximus::gpu {

textured_quad_s::textured_quad_s(shader_program_s* shader, uv_e uv)
    : shader_(shader)
{
    if (shader_ == nullptr) {
        throw std::invalid_argument("textured quad shader must not be null");
    }

    draw_state_.set_shader_program(shader_);
    if (uv == uv_e::flipped) {
        draw_state_.set_vertex_data(full_screen_quad_verts_flip_uv);
    } else {
        draw_state_.set_vertex_data(full_screen_quad_verts);
    }
}

textured_quad_s::batch_s::~batch_s()
{
    if (texture_bound_) {
        texture_s::unbind(0);
    }
}

void textured_quad_s::batch_s::draw(texture_s* texture, rect_s rect, double opacity)
{
    if (texture == nullptr) {
        return;
    }

    owner_->shader_->set_uniform("offset", rect.pos);
    owner_->shader_->set_uniform("scale", rect.size);
    owner_->shader_->set_uniform("opacity", opacity);

    texture->bind(0);
    texture_bound_ = true;
    owner_->draw_state_.draw();
}

void textured_quad_s::draw(texture_s* texture, rect_s rect, double opacity)
{
    auto batch = begin_batch();
    batch.draw(texture, rect, opacity);
}

void textured_quad_s::draw_mix(texture_s*  a,
                               texture_s*  b,
                               double      t,
                               rect_s      a_rect,
                               rect_s      b_rect,
                               mix_space_e mix_space)
{
    if (a == nullptr || b == nullptr) {
        return;
    }

    shader_->set_uniform("offset", vec2_t{0, 0});
    shader_->set_uniform("scale", vec2_t{1, 1});
    shader_->set_uniform("tex", 0);
    shader_->set_uniform("tex_b", 1);
    shader_->set_uniform("t", t);
    shader_->set_uniform("a_offset", a_rect.pos);
    shader_->set_uniform("a_scale", a_rect.size);
    shader_->set_uniform("b_offset", b_rect.pos);
    shader_->set_uniform("b_scale", b_rect.size);
    shader_->set_uniform("video_mix", mix_space == mix_space_e::video ? 1 : 0);

    a->bind(0);
    b->bind(1);
    draw_state_.draw();
    texture_s::unbind(1);
    texture_s::unbind(0);
}

} // namespace miximus::gpu
