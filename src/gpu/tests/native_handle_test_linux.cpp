#include "native_handle_test_support.hpp"

#include <array>
#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

namespace miximus::gpu::detail::test {

native_handle_s make_handle()
{
    std::array<int, 2> descriptors{};
    if (pipe(descriptors.data()) != 0) {
        throw std::system_error(errno, std::generic_category());
    }

    native_handle_s writer(descriptors[1]);

    return native_handle_s(descriptors[0]);
}

bool valid(native_handle_s::value_t handle) { return fcntl(handle, F_GETFD) >= 0; }

bool not_inherited(native_handle_s::value_t handle)
{
    const auto flags = fcntl(handle, F_GETFD);

    return flags >= 0 && (flags & FD_CLOEXEC) != 0;
}

} // namespace miximus::gpu::detail::test
