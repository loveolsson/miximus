#include "native_handle_test_support.hpp"

#include <system_error>
#include <windows.h>

namespace miximus::gpu::detail::test {

native_handle_s make_handle()
{
    auto value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (value == nullptr) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category());
    }

    return native_handle_s(value);
}

bool valid(native_handle_s::value_t handle)
{
    DWORD flags{};

    return GetHandleInformation(handle, &flags) != FALSE;
}

bool not_inherited(native_handle_s::value_t handle)
{
    DWORD flags{};

    return GetHandleInformation(handle, &flags) != FALSE && (flags & HANDLE_FLAG_INHERIT) == 0;
}

} // namespace miximus::gpu::detail::test
