#ifdef _WIN32

#include "platform_compat.hpp"

#include <cstddef>
#include <limits>

namespace miximus::nodes::decklink::detail {
namespace {
std::string bstr_to_utf8(BSTR value)
{
    if (value == nullptr) {
        return {};
    }

    const auto bstr_length = ::SysStringLen(value);
    if (bstr_length > static_cast<UINT>(std::numeric_limits<int>::max())) {
        return {};
    }

    const auto wide_length = static_cast<int>(bstr_length);
    const int  length      = ::WideCharToMultiByte(CP_UTF8, 0, value, wide_length, nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return {};
    }

    std::string result(static_cast<size_t>(length), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, value, wide_length, result.data(), length, nullptr, nullptr) != length) {
        return {};
    }
    return result;
}
} // namespace

decklink_ptr<IDeckLinkDiscovery> create_device_discovery()
{
    IDeckLinkDiscovery* discovery = nullptr;
    const auto          result    = ::CoCreateInstance(CLSID_CDeckLinkDiscovery,
                                           nullptr,
                                           CLSCTX_ALL,
                                           decklink_iid<IDeckLinkDiscovery>(),
                                           reinterpret_cast<void**>(&discovery));
    return SUCCEEDED(result) ? decklink_ptr(discovery, false) : decklink_ptr<IDeckLinkDiscovery>{};
}

decklink_ptr<IDeckLinkVideoConversion> create_video_conversion()
{
    IDeckLinkVideoConversion* conversion = nullptr;
    const auto                result     = ::CoCreateInstance(CLSID_CDeckLinkVideoConversion,
                                           nullptr,
                                           CLSCTX_ALL,
                                           decklink_iid<IDeckLinkVideoConversion>(),
                                           reinterpret_cast<void**>(&conversion));
    return SUCCEEDED(result) ? decklink_ptr(conversion, false) : decklink_ptr<IDeckLinkVideoConversion>{};
}

bool decklink_iid_equal(REFIID lhs, REFIID rhs) noexcept
{
    if (lhs.Data1 != rhs.Data1 || lhs.Data2 != rhs.Data2 || lhs.Data3 != rhs.Data3) {
        return false;
    }
    for (std::size_t i = 0; i < 8; ++i) {
        if (lhs.Data4[i] != rhs.Data4[i]) {
            return false;
        }
    }
    return true;
}

REFIID input_video_buffer_iid() noexcept
{
    static constexpr IID iid{
        0x4005D928, 0xBA67, 0x4663, {0xA7, 0xFA, 0xF4, 0x4A, 0xB8, 0x25, 0xC8, 0x09}
    };
    return iid;
}

std::string get_device_display_name(IDeckLink* device)
{
    BSTR raw_name = nullptr;
    if (device->GetDisplayName(&raw_name) != S_OK) {
        return {};
    }

    auto name = bstr_to_utf8(raw_name);
    ::SysFreeString(raw_name);
    return name;
}

std::string get_display_mode_name(IDeckLinkDisplayMode* mode)
{
    BSTR raw_name = nullptr;
    if (mode->GetName(&raw_name) != S_OK) {
        return {};
    }

    auto name = bstr_to_utf8(raw_name);
    ::SysFreeString(raw_name);
    return name;
}

} // namespace miximus::nodes::decklink::detail

#endif // _WIN32
