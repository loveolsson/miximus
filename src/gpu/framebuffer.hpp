#pragma once
#include "texture.hpp"

#include <memory>

namespace miximus::gpu {

class framebuffer_s
{
    GLuint                     id_{};
    GLuint                     rbo_id_{};
    std::unique_ptr<texture_s> owned_texture_;
    texture_s*                 texture_{};

    void initialize();

  public:
    enum class load_op_e
    {
        preserve,
        clear,
    };

    framebuffer_s(vec2i_t dimensions, texture_s::format_e color);
    explicit framebuffer_s(texture_s* texture);
    ~framebuffer_s();

    void        bind() const;
    void        begin_render(load_op_e load_op = load_op_e::preserve) const;
    void        begin_render(recti_s viewport, load_op_e load_op = load_op_e::preserve) const;
    void        blit(framebuffer_s* target) const;
    static void end_render();
    static void unbind();
    texture_s*  texture() { return texture_; }
    GLuint      id() { return id_; }
};

} // namespace miximus::gpu
