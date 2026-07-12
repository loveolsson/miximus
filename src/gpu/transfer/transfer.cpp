#include "transfer.hpp"

#include "detail/cuda.hpp"
#include "detail/dvp.hpp"
#include "detail/fallback.hpp"
#include "detail/persistent.hpp"
#include "gpu/context.hpp"
#include "logger/logger.hpp"

#include <string_view>

#ifdef _MSC_VER
#include <malloc.h>
#include <memory>
#include <stdexcept>
#define ALIGNED_ALLOC(a, s) _aligned_malloc(s, a)
#define ALIGNED_FREE(p) _aligned_free(p)
#else
#define ALIGNED_ALLOC(a, s) aligned_alloc(a, s)
#define ALIGNED_FREE(p) free(p)
#endif

namespace miximus::gpu::transfer {

transfer_i::transfer_i(size_t size, direction_e direction)
    : size_(size)
    , direction_(direction)
    , ptr_(nullptr)
{
}

void transfer_i::allocate_ptr() { ptr_ = ALIGNED_ALLOC(ALIGNMENT, size_); }

void transfer_i::free_ptr() { ALIGNED_FREE(ptr_); }

bool transfer_i::register_texture(type_e type, gpu::texture_s* texture)
{
    switch (type) {
        case type_e::dvp:
            return detail::dvp_transfer_s::register_texture_dvp(texture);
        default:
            return true;
    }
}

bool transfer_i::unregister_texture(type_e type, gpu::texture_s* texture)
{
    switch (type) {
        case type_e::dvp:
            return detail::dvp_transfer_s::unregister_texture_dvp(texture);
        default:
            return true;
    }
}

bool transfer_i::begin_texture_use(type_e type, gpu::texture_s* texture)
{
    switch (type) {
        case type_e::dvp:
            return detail::dvp_transfer_s::begin_texture_use_dvp(texture);
        default:
            return true;
    }
}

bool transfer_i::end_texture_use(type_e type, gpu::texture_s* texture)
{
    switch (type) {
        case type_e::dvp:
            return detail::dvp_transfer_s::end_texture_use_dvp(texture);
        default:
            return true;
    }
}

namespace {
transfer_i::type_e g_prefered_type = transfer_i::type_e::persistent;
bool               g_initialized   = false;
} // namespace

transfer_i::type_e transfer_i::get_prefered_type() { return g_prefered_type; }

void transfer_i::initialize_preferred_type()
{
    if (g_initialized) {
        return;
    }
    g_initialized = true;

    const GLubyte*         renderer = glGetString(GL_RENDERER);
    const std::string_view renderer_view(reinterpret_cast<const char*>(renderer));

    [[maybe_unused]] const bool has_dvp        = renderer_view.find("Quadro") != std::string_view::npos;
    [[maybe_unused]] const bool has_amd_pinned = gpu::context_s::has_extension("GL_AMD_pinned_memory");

    if (has_dvp) {
        if (detail::dvp_transfer_s::initialize_context()) {
            getlog("gpu")->info("Transfer: DVP initialised — using GPU Direct transfers");
            g_prefered_type = type_e::dvp;
            return;
        }
    }

    if (detail::cuda_transfer_s::initialize_context()) {
        getlog("gpu")->info("Transfer: using CUDA/OpenGL interoperability");
        g_prefered_type = type_e::cuda;
        return;
    }

    // TODO(Love): Implement AMD pinned memory path
    g_prefered_type = type_e::persistent;
}

void transfer_i::shutdown()
{
    detail::cuda_transfer_s::shutdown_context();
    detail::dvp_transfer_s::shutdown_context();

    g_prefered_type = type_e::persistent;
    g_initialized   = false;
}

std::unique_ptr<transfer_i>
transfer_i::create_transfer(transfer_i::type_e type, size_t size, transfer_i::direction_e dir)
{
    switch (type) {
        case type_e::basic:
            return std::make_unique<detail::fallback_transfer_s>(size, dir);

        case type_e::persistent:
            return std::make_unique<detail::pinned_transfer_s>(size, dir);

        case type_e::cuda:
            return std::make_unique<detail::cuda_transfer_s>(size, dir);

        case type_e::dvp:
            return std::make_unique<detail::dvp_transfer_s>(size, dir);

        default:
            throw std::runtime_error("Attempting to create not yet implemented transfer type");
            return nullptr;
    }
}

} // namespace miximus::gpu::transfer
