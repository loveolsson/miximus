#include "backend.hpp"

#ifdef _MSC_VER
#include <malloc.h>
#define ALIGNED_ALLOC(a, s) _aligned_malloc(s, a)
#define ALIGNED_FREE(p) _aligned_free(p)
#else
#include <cstdlib>
#define ALIGNED_ALLOC(a, s) aligned_alloc(a, s)
#define ALIGNED_FREE(p) free(p)
#endif

namespace miximus::gpu::transfer::detail {

backend_i::backend_i(size_t size, direction_e direction)
    : size_(size)
    , direction_(direction)
{
}

backend_i::~backend_i() = default;

bool backend_i::bind_texture(texture_s* texture)
{
    if (texture_ != nullptr || texture == nullptr || !register_texture_impl(texture)) {
        return false;
    }
    texture_ = texture;
    return true;
}

bool backend_i::unbind_texture()
{
    if (texture_ == nullptr) {
        return true;
    }
    const bool success = unregister_texture_impl(texture_);
    texture_           = nullptr;
    return success;
}

bool backend_i::begin_texture_use() { return texture_ != nullptr && begin_texture_use_impl(texture_); }

bool backend_i::end_texture_use() { return texture_ != nullptr && end_texture_use_impl(texture_); }

void backend_i::allocate_data() { data_ = ALIGNED_ALLOC(TRANSFER_ALIGNMENT, size_); }

void backend_i::free_data()
{
    ALIGNED_FREE(data_);
    data_ = nullptr;
}

} // namespace miximus::gpu::transfer::detail
