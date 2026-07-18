#include "platform_compat.hpp"

#include <cstdlib>

namespace miximus::nodes::decklink::detail {

decklink_ptr<IDeckLinkDiscovery> create_device_discovery()
{
    return decklink_ptr(CreateDeckLinkDiscoveryInstance(), false);
}

decklink_ptr<IDeckLinkVideoConversion> create_video_conversion()
{
    return decklink_ptr(CreateVideoConversionInstance(), false);
}

std::string get_device_display_name(IDeckLink* device)
{
    const char* raw_name = nullptr;
    if (device->GetDisplayName(&raw_name) != S_OK) {
        return {};
    }

    std::string name(raw_name);
    std::free(const_cast<char*>(raw_name));
    return name;
}

std::string get_display_mode_name(IDeckLinkDisplayMode* mode)
{
    const char* raw_name = nullptr;
    if (mode->GetName(&raw_name) != S_OK) {
        return {};
    }

    std::string name(raw_name);
    std::free(const_cast<char*>(raw_name));
    return name;
}

} // namespace miximus::nodes::decklink::detail
