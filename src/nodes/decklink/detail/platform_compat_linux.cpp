#if !defined(_WIN32) && !defined(__APPLE__)

#include "platform_compat.hpp"

#include <cstdlib>

namespace miximus::nodes::decklink::detail {

namespace {
void free_sdk_string(const char* value)
{
    // The Linux SDK transfers ownership while exposing the allocation through a pointer-to-const.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    std::free(const_cast<char*>(value));
}
} // namespace

decklink_ptr<IDeckLinkDiscovery> create_device_discovery()
{
    return decklink_ptr(CreateDeckLinkDiscoveryInstance(), false);
}

decklink_ptr<IDeckLinkVideoConversion> create_video_conversion()
{
    return decklink_ptr(CreateVideoConversionInstance(), false);
}

bool decklink_iid_equal(REFIID lhs, REFIID rhs) noexcept
{
    return lhs.byte0 == rhs.byte0 && lhs.byte1 == rhs.byte1 && lhs.byte2 == rhs.byte2 && lhs.byte3 == rhs.byte3 &&
           lhs.byte4 == rhs.byte4 && lhs.byte5 == rhs.byte5 && lhs.byte6 == rhs.byte6 && lhs.byte7 == rhs.byte7 &&
           lhs.byte8 == rhs.byte8 && lhs.byte9 == rhs.byte9 && lhs.byte10 == rhs.byte10 && lhs.byte11 == rhs.byte11 &&
           lhs.byte12 == rhs.byte12 && lhs.byte13 == rhs.byte13 && lhs.byte14 == rhs.byte14 && lhs.byte15 == rhs.byte15;
}

REFIID upload_video_buffer_iid() noexcept
{
    return {0x40, 0x05, 0xD9, 0x28, 0xBA, 0x67, 0x46, 0x63, 0xA7, 0xFA, 0xF4, 0x4A, 0xB8, 0x25, 0xC8, 0x09};
}

std::string get_device_display_name(IDeckLink* device)
{
    const char* raw_name = nullptr;
    if (device->GetDisplayName(&raw_name) != S_OK) {
        return {};
    }

    std::string name(raw_name);
    free_sdk_string(raw_name);
    return name;
}

std::string get_display_mode_name(IDeckLinkDisplayMode* mode)
{
    const char* raw_name = nullptr;
    if (mode->GetName(&raw_name) != S_OK) {
        return {};
    }

    std::string name(raw_name);
    free_sdk_string(raw_name);
    return name;
}

} // namespace miximus::nodes::decklink::detail

#endif // !defined(_WIN32) && !defined(__APPLE__)
