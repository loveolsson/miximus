#include "nodes/disconnected_value_provider.hpp"

#include "core/app_state.hpp"
#include "gpu/texture.hpp"

#include <memory>

namespace miximus::nodes::detail {

disconnected_value_provider_s<framebuffer_source_info_s>::disconnected_value_provider_s()  = default;
disconnected_value_provider_s<framebuffer_source_info_s>::~disconnected_value_provider_s() = default;

void disconnected_value_provider_s<framebuffer_source_info_s>::release() noexcept { framebuffer_.reset(); }

framebuffer_source_info_s
disconnected_value_provider_s<framebuffer_source_info_s>::get(core::app_state_s*               app,
                                                              const framebuffer_source_info_s& fallback,
                                                              std::string_view                 name)
{
    if (app == nullptr) {
        return fallback;
    }

    const auto& dimensions = app->frame_settings().framebuffer.default_size;
    if (!framebuffer_ || framebuffer_->dimensions() != dimensions) {
        framebuffer_ = std::make_unique<gpu::texture_s>(*app->gpu(), dimensions, gpu::format_e::rgba_unorm16);
    }

    framebuffer_->clear(app->commands());

    return {.texture = framebuffer_.get(), .name = name};
}

} // namespace miximus::nodes::detail
