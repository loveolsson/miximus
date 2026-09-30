#pragma once

#include "gpu/detail/native_handle.hpp"

namespace miximus::gpu::detail::test {

native_handle_s make_handle();
bool            valid(native_handle_s::value_t handle);
bool            not_inherited(native_handle_s::value_t handle);

} // namespace miximus::gpu::detail::test
