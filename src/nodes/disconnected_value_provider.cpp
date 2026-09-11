#include "nodes/disconnected_value_provider.hpp"

#include "core/app_state.hpp"
#include "gpu/texture.hpp"

#include <memory>

namespace miximus::nodes::detail {

disconnected_value_provider_s<gpu::texture_s*>::~disconnected_value_provider_s() = default;

void disconnected_value_provider_s<gpu::texture_s*>::release() noexcept { framebuffer_.reset(); }

gpu::texture_s* disconnected_value_provider_s<gpu::texture_s*>::get(core::app_state_s*     app,
                                                                    gpu::texture_s* const& fallback)
{
    if (app == nullptr) {
        return fallback;
    }

    const auto& dimensions = app->frame_settings().framebuffer.default_size;
    if (!framebuffer_ || framebuffer_->dimensions() != dimensions) {
        framebuffer_ = std::make_unique<gpu::texture_s>(*app->gpu(), dimensions, gpu::format_e::rgba_unorm16);
    }

    framebuffer_->clear(app->commands());

    return framebuffer_.get();
}

} // namespace miximus::nodes::detail
