#pragma once
#include <Processing.NDI.Lib.h>
#include <cstddef>
#include <cstdint>
#include <span>

namespace miximus::ndi_sdk {

// NDI's send API does not provide a const-qualified frame-data field, even
// though sending only consumes the supplied buffer.
inline uint8_t* send_buffer(std::span<const std::byte> bytes) noexcept
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    return const_cast<uint8_t*>(reinterpret_cast<const uint8_t*>(bytes.data()));
}

} // namespace miximus::ndi_sdk
