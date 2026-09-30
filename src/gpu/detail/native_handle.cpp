#include "native_handle.hpp"

#include <utility>

namespace miximus::gpu::detail {

native_handle_s::~native_handle_s() { reset(); }

native_handle_s::native_handle_s(native_handle_s&& other) noexcept
    : value_(other.release())
{
}

native_handle_s& native_handle_s::operator=(native_handle_s&& other) noexcept
{
    if (this != &other) {
        reset(other.release());
    }

    return *this;
}

native_handle_s::value_t native_handle_s::release() noexcept { return std::exchange(value_, invalid); }

} // namespace miximus::gpu::detail
