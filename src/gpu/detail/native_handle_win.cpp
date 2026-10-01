#include "native_handle.hpp"

#include <system_error>
#include <windows.h>

namespace miximus::gpu::detail {

void native_handle_s::reset(value_t value) noexcept
{
    if (value_ != invalid) {
        CloseHandle(value_);
    }

    value_ = value;
}

native_handle_s native_handle_s::duplicate(value_t borrowed)
{
    HANDLE copy{};
    if (DuplicateHandle(GetCurrentProcess(), borrowed, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS) ==
        FALSE) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "duplicate native handle");
    }

    return native_handle_s(copy);
}

} // namespace miximus::gpu::detail
