#include "nodes/disconnected_value_provider.hpp"

#include "gpu/texture.hpp"
#include "nodes/execution_context.hpp"

#include <memory>

namespace miximus::nodes::detail {

disconnected_value_provider_s<framebuffer_source_info_s>::disconnected_value_provider_s()  = default;
disconnected_value_provider_s<framebuffer_source_info_s>::~disconnected_value_provider_s() = default;

void disconnected_value_provider_s<framebuffer_source_info_s>::release() noexcept { framebuffer_.reset(); }

framebuffer_source_info_s
disconnected_value_provider_s<framebuffer_source_info_s>::get(execution_context_s*             context,
                                                              const framebuffer_source_info_s& fallback,
                                                              std::string_view                 name)
{
    if (context == nullptr) {
        return fallback;
    }

    const auto& dimensions = context->frame_settings().framebuffer.default_size;
    if (!framebuffer_ || framebuffer_->dimensions() != dimensions) {
        framebuffer_ = std::make_unique<gpu::texture_s>(*context->gpu(), dimensions, gpu::format_e::rgba_unorm16);
    }

    framebuffer_->clear(context->commands());

    return {.texture = framebuffer_.get(), .name = name};
}

} // namespace miximus::nodes::detail
