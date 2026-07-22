#pragma once

#include <cstdint>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace miximus::utils {

inline uint64_t process_id()
{
#ifdef _WIN32
    return static_cast<uint64_t>(::_getpid());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

} // namespace miximus::utils
