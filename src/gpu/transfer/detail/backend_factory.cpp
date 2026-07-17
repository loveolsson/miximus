#include "backend_factory.hpp"

#include "cuda.hpp"
#include "dvp.hpp"
#include "gpu/context.hpp"
#include "logger/logger.hpp"
#include "persistent.hpp"

#include <cstdint>
#include <string_view>

namespace miximus::gpu::transfer::detail {
namespace {
enum class backend_type_e : std::uint8_t
{
    persistent,
    cuda,
    dvp,
};

struct backend_state_s
{
    backend_type_e type{backend_type_e::persistent};
    bool           initialized{};
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
    const bool has_dvp = renderer_view.find("Quadro") != std::string_view::npos;

    if (has_dvp && dvp_transfer_s::initialize_context()) {
        getlog("gpu")->info("Transfer: DVP initialised — using GPU Direct transfers");
        state.type = backend_type_e::dvp;
        return;
    }
    if (cuda_transfer_s::initialize_context()) {
        getlog("gpu")->info("Transfer: using CUDA/OpenGL interoperability");
        state.type = backend_type_e::cuda;
        return;
    }
    state.type = backend_type_e::persistent;
}

void shutdown_backends()
{
    cuda_transfer_s::shutdown_context();
    dvp_transfer_s::shutdown_context();

    auto& state       = backend_state();
    state.type        = backend_type_e::persistent;
    state.initialized = false;
}

std::unique_ptr<backend_i> create_backend(size_t size, backend_i::direction_e direction)
{
    switch (backend_state().type) {
        case backend_type_e::persistent:
            return std::make_unique<pinned_transfer_s>(size, direction);
        case backend_type_e::cuda:
            return std::make_unique<cuda_transfer_s>(size, direction);
        case backend_type_e::dvp:
            return std::make_unique<dvp_transfer_s>(size, direction);
    }
    return nullptr;
}

} // namespace miximus::gpu::transfer::detail
