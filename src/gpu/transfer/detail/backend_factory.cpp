#include "backend_factory.hpp"

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
struct backend_state_s
{
    bool initialized{};
    bool dvp_available{};
    bool cuda_available{};
};

backend_state_s& backend_state()
{
    static backend_state_s state;
    return state;
}
} // namespace

void initialize_backends()
{
    auto& state = backend_state();
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

void shutdown_backends()
{
    cuda_transfer_s::shutdown_context();
    dvp_transfer_s::shutdown_context();

    backend_state() = {};
}

backend_result_s create_backend(const texture_transfer_requirements_s& requirements,
                                backend_i::direction_e                 direction,
                                texture_s*                             texture)
{
    if (texture == nullptr) {
        throw std::invalid_argument("transfer backend requires a texture");
    }

    const auto try_bind = [texture, alignment = requirements.address_alignment](
                              std::unique_ptr<backend_i> backend) -> std::unique_ptr<backend_i> {
        if (reinterpret_cast<std::uintptr_t>(backend->data()) % alignment != 0) {
            return nullptr;
        }
        if (!backend->bind_texture(texture)) {
            return nullptr;
        }
        return backend;
    };

    auto& state = backend_state();
    if (state.dvp_available && dvp_transfer_s::supports(requirements)) {
        try {
            if (auto backend = try_bind(std::make_unique<dvp_transfer_s>(requirements, direction))) {
                getlog("gpu")->debug("Selected DVP direct-memory transfer backend");
                return {
                    .backend          = std::move(backend),
                    .type             = backend_type_e::dvp,
                    .path             = transfer_path_e::direct_memory,
                    .allocation_bytes = requirements.byte_size,
                };
            }
            getlog("gpu")->warn("DVP texture registration failed; trying another transfer backend");
        } catch (const std::exception& error) {
            getlog("gpu")->warn("DVP transfer allocation failed: {}; trying another backend", error.what());
        }
    }

    if (state.cuda_available) {
        const bool direct_image = cuda_transfer_s::supports_direct_image(requirements.format);
        if (direct_image) {
            try {
                if (auto backend = try_bind(std::make_unique<cuda_transfer_s>(requirements, direction, true))) {
                    getlog("gpu")->debug("Selected CUDA direct-image transfer backend");
                    return {
                        .backend          = std::move(backend),
                        .type             = backend_type_e::cuda,
                        .path             = transfer_path_e::direct_image,
                        .allocation_bytes = requirements.byte_size,
                    };
                }
                getlog("gpu")->debug("CUDA direct image registration failed; trying the CUDA pixel-buffer path");
            } catch (const std::exception& error) {
                getlog("gpu")->warn("CUDA direct image allocation failed: {}; trying the pixel-buffer path",
                                    error.what());
            }
        }

        try {
            if (auto backend = try_bind(std::make_unique<cuda_transfer_s>(requirements, direction, false))) {
                getlog("gpu")->debug("Selected CUDA pixel-buffer transfer backend");
                return {
                    .backend          = std::move(backend),
                    .type             = backend_type_e::cuda,
                    .path             = transfer_path_e::pixel_buffer,
                    .allocation_bytes = requirements.byte_size * 2,
                };
            }
        } catch (const std::exception& error) {
            getlog("gpu")->warn("CUDA pixel-buffer allocation failed: {}; using persistent OpenGL transfer",
                                error.what());
        }
    }

    auto backend = try_bind(std::make_unique<pinned_transfer_s>(requirements, direction));
    if (!backend) {
        throw std::runtime_error("failed to bind persistent transfer backend to texture");
    }
    getlog("gpu")->debug("Selected persistent OpenGL pixel-buffer transfer backend");
    return {
        .backend          = std::move(backend),
        .type             = backend_type_e::persistent,
        .path             = transfer_path_e::pixel_buffer,
        .allocation_bytes = requirements.byte_size,
    };
}

} // namespace miximus::gpu::transfer::detail
