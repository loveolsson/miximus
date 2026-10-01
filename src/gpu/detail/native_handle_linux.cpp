#include "native_handle.hpp"

#include <cerrno>
#include <fcntl.h>
#include <system_error>
#include <unistd.h>

namespace miximus::gpu::detail {

void native_handle_s::reset(value_t value) noexcept
{
    if (value_ != invalid) {
        ::close(value_);
    }

    value_ = value;
}

native_handle_s native_handle_s::duplicate(value_t borrowed)
{
    const int copy = fcntl(borrowed, F_DUPFD_CLOEXEC, 0);
    if (copy < 0) {
        throw std::system_error(errno, std::generic_category(), "duplicate native handle");
    }

    return native_handle_s(copy);
}

} // namespace miximus::gpu::detail
