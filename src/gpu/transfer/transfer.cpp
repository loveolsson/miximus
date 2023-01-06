#include "transfer.hpp"
#include "detail/fallback.hpp"
#include "detail/persistent.hpp"
#include "gpu/context.hpp"

#include <string_view>

#ifdef _MSC_VER
#include <malloc.h>
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

bool transfer_i::register_texture(type_e type, gpu::texture_s* /*texture*/)
{
    switch (type) {
        default:
            return true;
    }
}

bool transfer_i::unregister_texture(type_e type, gpu::texture_s* /*texture*/)
{
    switch (type) {
        default:
            return true;
    }
}

bool transfer_i::begin_texture_use(type_e type, gpu::texture_s* /*texture*/)
{
    switch (type) {
        default:
            return true;
    }
}

bool transfer_i::end_texture_use(type_e type, gpu::texture_s* /*texture*/)
{
    switch (type) {
        default:
            return true;
    }
}

transfer_i::type_e transfer_i::get_prefered_type()
{
    static type_e type = []() {
        const GLubyte*   renderer = glGetString(GL_RENDERER);
        std::string_view renderer_view(reinterpret_cast<const char*>(renderer));

        bool has_dvp        = renderer_view.find("Quadro") != std::string_view::npos;
        bool has_amd_pinned = gpu::context_s::has_extension("GL_AMD_pinned_memory");

        // TODO(Love): Implement

        return type_e::persistent;
    }();
    return type;
}

std::unique_ptr<transfer_i>
transfer_i::create_transfer(transfer_i::type_e type, size_t size, transfer_i::direction_e dir)
{
    switch (type) {
        case type_e::basic:
            return std::make_unique<detail::fallback_transfer_s>(size, dir);

        case type_e::persistent:
            return std::make_unique<detail::pinned_transfer_s>(size, dir);

        default:
            throw std::runtime_error("Attempting to create not yet implemented transfer type");
            return nullptr;
    }
}

} // namespace miximus::gpu::transfer
