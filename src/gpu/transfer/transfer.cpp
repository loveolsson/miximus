#include "transfer.hpp"
#include "detail/fallback.hpp"
#include "detail/pinned.hpp"

namespace miximus::gpu::transfer {

transfer_i::transfer_i(size_t size, direction_e direction)
    : size_(size)
    , direction_(direction)
    , ptr_(aligned_alloc(16, size))
{
}

transfer_i::~transfer_i() { free(ptr_); }

bool transfer_i::register_texture(type_e type, const texture_s& texture)
{
    switch (type) {
        default:
            return true;
    }
}

bool transfer_i::unregister_texture(type_e type, const texture_s& texture)
{
    switch (type) {
        default:
            return true;
    }
}

bool transfer_i::begin_texture_use(type_e type, const texture_s& texture)
{
    switch (type) {
        default:
            return true;
    }
}

bool transfer_i::end_texture_use(type_e type, const texture_s& texture)
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
