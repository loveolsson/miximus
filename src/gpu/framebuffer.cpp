#include "framebuffer.hpp"

#include "context.hpp"
#include "logger/logger.hpp"

#include <array>
#include <memory>

namespace miximus::gpu {

framebuffer_s::framebuffer_s(vec2i_t                     dimensions,
                             texture_s::storage_format_e storage_format,
                             texture_s::sampling_e       sampling)
    : owned_texture_(
          std::make_unique<texture_s>(dimensions, storage_format, input_component_mapping_e::identity, sampling))
    , texture_(owned_texture_.get())
{
    initialize();
}

framebuffer_s::framebuffer_s(texture_s* texture)
    : texture_(texture)
{
    initialize();
}

void framebuffer_s::initialize()
{
    glGenFramebuffers(1, &id_);
    bind();

    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture_->id(), 0);

    auto tex_dims = texture_->texture_dimensions();

    glGenRenderbuffers(1, &rbo_id_);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo_id_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, tex_dims.x, tex_dims.y);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo_id_);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        getlog("gpu")->error("Framebuffer is not complete");
    }

    unbind();
}

framebuffer_s::~framebuffer_s()
{
    if (!context_s::require_current()) {
        return;
    }
    glDeleteFramebuffers(1, &id_);
    glDeleteRenderbuffers(1, &rbo_id_);
}

void framebuffer_s::bind() const { glBindFramebuffer(GL_FRAMEBUFFER, id_); }

void framebuffer_s::begin_render(load_op_e load_op) const
{
    const auto dimensions = texture_->texture_dimensions();
    begin_render(
        {
            .pos  = {0, 0},
            .size = dimensions,
    },
        load_op);
}

void framebuffer_s::begin_render(recti_s viewport, load_op_e load_op) const
{
    bind();
    glViewport(viewport.pos.x, viewport.pos.y, viewport.size.x, viewport.size.y);

    if (load_op == load_op_e::clear) {
        if (texture_s::storage_format_info(texture_->storage_format()).integer) {
            constexpr std::array<GLuint, 4> clear_color{};
            glClearBufferuiv(GL_COLOR, 0, clear_color.data());
            glClear(static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT));
        } else {
            glClearColor(0, 0, 0, 0);
            glClear(static_cast<GLbitfield>(GL_COLOR_BUFFER_BIT) | static_cast<GLbitfield>(GL_DEPTH_BUFFER_BIT));
        }
    }
}

void framebuffer_s::end_render() { unbind(); }

void framebuffer_s::unbind() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

} // namespace miximus::gpu
