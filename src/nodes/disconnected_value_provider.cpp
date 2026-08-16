#include "nodes/disconnected_value_provider.hpp"

#include "core/app_state.hpp"
#include "gpu/framebuffer.hpp"
#include "gpu/texture.hpp"

#include <memory>

namespace miximus::nodes::detail {

disconnected_value_provider_s<gpu::framebuffer_s*>::~disconnected_value_provider_s() = default;

void disconnected_value_provider_s<gpu::framebuffer_s*>::release() noexcept { framebuffer_.reset(); }

gpu::framebuffer_s* disconnected_value_provider_s<gpu::framebuffer_s*>::get(core::app_state_s*         app,
                                                                            gpu::framebuffer_s* const& fallback)
{
    if (app == nullptr) {
        return fallback;
    }

    const auto& dimensions = app->frame_settings().framebuffer.default_size;
    if (!framebuffer_ || framebuffer_->texture()->texture_dimensions() != dimensions) {
        framebuffer_ = std::make_unique<gpu::framebuffer_s>(dimensions, gpu::texture_s::pixel_format_e::rgba_f16);
    }

    framebuffer_->begin_render(gpu::framebuffer_s::load_op_e::clear);
    gpu::framebuffer_s::end_render();

    return framebuffer_.get();
}

} // namespace miximus::nodes::detail
