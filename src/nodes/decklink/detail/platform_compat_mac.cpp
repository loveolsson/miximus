#include "platform_compat.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <vector>

namespace miximus::nodes::decklink::detail {
namespace {
std::string cf_string_to_utf8(CFStringRef value)
{
    if (value == nullptr) {
        return {};
    }

    const auto        length = CFStringGetMaximumSizeForEncoding(CFStringGetLength(value), kCFStringEncodingUTF8) + 1;
    std::vector<char> buffer(static_cast<size_t>(length));
    if (!CFStringGetCString(value, buffer.data(), length, kCFStringEncodingUTF8)) {
        return {};
    }
    return buffer.data();
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

std::string get_device_display_name(IDeckLink* device)
{
    CFStringRef raw_name = nullptr;
    if (device->GetDisplayName(&raw_name) != S_OK) {
        return {};
    }

    auto name = cf_string_to_utf8(raw_name);
    CFRelease(raw_name);
    return name;
}

std::string get_display_mode_name(IDeckLinkDisplayMode* mode)
{
    CFStringRef raw_name = nullptr;
    if (mode->GetName(&raw_name) != S_OK) {
        return {};
    }

    auto name = cf_string_to_utf8(raw_name);
    CFRelease(raw_name);
    return name;
}

} // namespace miximus::nodes::decklink::detail
