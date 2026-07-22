#pragma once
#include "draw_state.hpp"
#include "texture_fwd.hpp"
#include "types.hpp"

#include <utility>

namespace miximus::gpu {

class textured_quad_s
{
  public:
    enum class uv_e
    {
        regular,
        flipped,
    };

    enum class mix_space_e
    {
        linear,
        video,
    };

    class batch_s
    {
        textured_quad_s* owner_{};
        bool             texture_bound_{};

        explicit batch_s(textured_quad_s* owner)
            : owner_(owner)
        {
        }

        friend class textured_quad_s;

      public:
        ~batch_s();

        batch_s(batch_s&& other) noexcept
            : owner_(std::exchange(other.owner_, nullptr))
            , texture_bound_(std::exchange(other.texture_bound_, false))
        {
        }

        batch_s(const batch_s&)            = delete;
        batch_s& operator=(const batch_s&) = delete;
        batch_s& operator=(batch_s&&)      = delete;

        void draw(texture_s* texture, rect_s rect = {}, double opacity = 1.0);
    };

  private:
    draw_state_s      draw_state_;
    shader_program_s* shader_{};

  public:
    explicit textured_quad_s(shader_program_s* shader, uv_e uv = uv_e::flipped);

    textured_quad_s(const textured_quad_s&)            = delete;
    textured_quad_s(textured_quad_s&&)                 = delete;
    textured_quad_s& operator=(const textured_quad_s&) = delete;
    textured_quad_s& operator=(textured_quad_s&&)      = delete;

    shader_program_s* shader() const { return shader_; }
    batch_s           begin_batch() { return batch_s(this); }
    void              draw(texture_s* texture, rect_s rect = {}, double opacity = 1.0);
    void draw_mix(texture_s* a, texture_s* b, double t, rect_s a_rect, rect_s b_rect, mix_space_e mix_space);
};

} // namespace miximus::gpu
