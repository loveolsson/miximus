#include "transfer.hpp"
#include "detail/fallback.hpp"
#include "detail/pinned.hpp"

#ifdef _MSC_VER
#include <malloc.h>
#define aligned_malloc_impl(a, s) _aligned_malloc(s, a)
#define aligned_free_impl(p) _aligned_free(p)
#else
#include <stdlib.h>
#define aligned_malloc_impl(a, s) aligned_malloc(a, s)
#define aligned_free_impl(p) free(p)
#endif

namespace miximus::gpu::transfer {

transfer_i::transfer_i(size_t size, direction_e direction)
    : size_(size)
    , direction_(direction)
    , ptr_(aligned_malloc_impl(ALIGNMENT, size))
{
}

transfer_i::~transfer_i()
{
    aligned_free_impl(ptr_); //
}

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

transfer_i::type_e transfer_i::get_prefered_type() { return type_e::pinned; }

std::unique_ptr<transfer_i>
transfer_i::create_transfer(transfer_i::type_e type, size_t size, transfer_i::direction_e dir)
{
    switch (type) {
        case type_e::basic:
            return std::make_unique<detail::fallback_transfer_s>(size, dir);

        case type_e::pinned:
            return std::make_unique<detail::pinned_transfer_s>(size, dir);

        default:
            throw std::runtime_error("Attempting to create not yet implemented transfer type");
            return nullptr;
    }
}

} // namespace miximus::gpu::transfer
