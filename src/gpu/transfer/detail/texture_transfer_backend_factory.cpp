#include "texture_transfer_backend_factory.hpp"

#include "cuda.hpp"
#include "dvp.hpp"
#include "gpu/context.hpp"
#include "logger/logger.hpp"
#include "persistent.hpp"

#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string_view>

namespace miximus::gpu::transfer::detail {
namespace {
struct texture_transfer_capabilities_s
{
    bool initialized{};
    bool dvp_available{};
    bool cuda_available{};
};

texture_transfer_capabilities_s& texture_transfer_capabilities()
{
    static texture_transfer_capabilities_s state;
    return state;
}
} // namespace

void initialize_texture_transfer_backends()
{
    auto& state = texture_transfer_capabilities();
    if (state.initialized) {
        return;
    }
    state.initialized = true;

    const GLubyte*         renderer = glGetString(GL_RENDERER);
    const std::string_view renderer_view =
        renderer != nullptr ? std::string_view(reinterpret_cast<const char*>(renderer)) : std::string_view{};
    const bool has_nvidia = renderer_view.find("NVIDIA") != std::string_view::npos;

    if (has_nvidia && dvp_transfer_s::initialize_context()) {
        state.dvp_available = true;
        getlog("gpu")->info("Transfer: DVP available for compatible streams");
    }
    if (cuda_transfer_s::initialize_context()) {
        state.cuda_available = true;
        getlog("gpu")->info("Transfer: CUDA/OpenGL interoperability available for compatible streams");
    }

    if (!state.dvp_available && !state.cuda_available) {
        getlog("gpu")->info("Transfer: using persistent OpenGL pixel buffers");
    }
}

void shutdown_texture_transfer_backends()
{
    cuda_transfer_s::shutdown_context();
    dvp_transfer_s::shutdown_context();

    texture_transfer_capabilities() = {};
}

texture_transfer_backend_selection_s create_texture_transfer_backend(const texture_transfer_layout_s& transfer_layout,
                                                                     texture_transfer_backend_i::direction_e direction,
                                                                     texture_s*                              texture)
{
    if (texture == nullptr) {
        throw std::invalid_argument("transfer backend requires a texture");
    }

    const auto try_register =
        [texture, alignment = transfer_layout.host_address_alignment_bytes](
            std::unique_ptr<texture_transfer_backend_i> candidate) -> std::unique_ptr<texture_transfer_backend_i> {
        if (reinterpret_cast<std::uintptr_t>(candidate->host_memory()) % alignment != 0) {
            return nullptr;
        }
        if (!candidate->register_texture(texture)) {
            return nullptr;
        }
        return candidate;
    };

    auto& state = texture_transfer_capabilities();
    if (state.dvp_available && dvp_transfer_s::supports(transfer_layout)) {
        try {
            if (auto selected_backend = try_register(std::make_unique<dvp_transfer_s>(transfer_layout, direction))) {
                getlog("gpu")->debug("Selected DVP direct-memory transfer backend");
                return {
                    .transfer_backend         = std::move(selected_backend),
                    .backend_kind             = texture_transfer_backend_kind_e::dvp,
                    .memory_path              = texture_transfer_memory_path_e::direct_memory,
                    .backend_allocation_bytes = transfer_layout.host_buffer_size_bytes,
                };
            }
            getlog("gpu")->warn("DVP texture registration failed; trying another transfer backend");
        } catch (const std::exception& error) {
            getlog("gpu")->warn("DVP transfer allocation failed: {}; trying another backend", error.what());
        }
    }

    if (state.cuda_available) {
        const bool direct_image = cuda_transfer_s::supports_direct_image(transfer_layout.pixel_format);
        if (direct_image) {
            try {
                if (auto selected_backend =
                        try_register(std::make_unique<cuda_transfer_s>(transfer_layout, direction, true))) {
                    getlog("gpu")->debug("Selected CUDA direct-image transfer backend");
                    return {
                        .transfer_backend         = std::move(selected_backend),
                        .backend_kind             = texture_transfer_backend_kind_e::cuda,
                        .memory_path              = texture_transfer_memory_path_e::direct_image,
                        .backend_allocation_bytes = transfer_layout.host_buffer_size_bytes,
                    };
                }
                getlog("gpu")->debug("CUDA direct image registration failed; trying the CUDA pixel-buffer path");
            } catch (const std::exception& error) {
                getlog("gpu")->warn("CUDA direct image allocation failed: {}; trying the pixel-buffer path",
                                    error.what());
            }
        }

        try {
            if (auto selected_backend =
                    try_register(std::make_unique<cuda_transfer_s>(transfer_layout, direction, false))) {
                getlog("gpu")->debug("Selected CUDA pixel-buffer transfer backend");
                return {
                    .transfer_backend         = std::move(selected_backend),
                    .backend_kind             = texture_transfer_backend_kind_e::cuda,
                    .memory_path              = texture_transfer_memory_path_e::pixel_buffer,
                    .backend_allocation_bytes = transfer_layout.host_buffer_size_bytes * 2,
                };
            }
        } catch (const std::exception& error) {
            getlog("gpu")->warn("CUDA pixel-buffer allocation failed: {}; using persistent OpenGL transfer",
                                error.what());
        }
    }

    auto selected_backend = try_register(std::make_unique<pinned_transfer_s>(transfer_layout, direction));
    if (!selected_backend) {
        throw std::runtime_error("failed to register persistent transfer backend with texture");
    }
    getlog("gpu")->debug("Selected persistent OpenGL pixel-buffer transfer backend");
    return {
        .transfer_backend         = std::move(selected_backend),
        .backend_kind             = texture_transfer_backend_kind_e::persistent,
        .memory_path              = texture_transfer_memory_path_e::pixel_buffer,
        .backend_allocation_bytes = transfer_layout.host_buffer_size_bytes,
    };
}

} // namespace miximus::gpu::transfer::detail
