#include "framebuffer.hpp"
#include "logger/logger.hpp"

namespace miximus::gpu {

framebuffer_s::framebuffer_s(vec2i_t dimensions, texture_s::colorspace_e color)
{
    glGenFramebuffers(1, &id_);
    bind();

    texture_ = std::make_unique<texture_s>(dimensions, color);
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
    glDeleteFramebuffers(1, &id_);
    glDeleteRenderbuffers(1, &rbo_id_);
}

void framebuffer_s::bind() const { glBindFramebuffer(GL_FRAMEBUFFER, id_); }

void framebuffer_s::unbind() { glBindFramebuffer(GL_FRAMEBUFFER, 0); }

} // namespace miximus::gpu