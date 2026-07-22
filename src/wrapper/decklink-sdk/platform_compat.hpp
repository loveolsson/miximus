#pragma once
#include "decklink_ptr.hpp"

#include <string>

#ifdef _WIN32
#ifdef MIXIMUS_DECKLINK_WRAPPER_BUILD
#define MIXIMUS_DECKLINK_API __declspec(dllexport)
#else
#define MIXIMUS_DECKLINK_API __declspec(dllimport)
#endif
#else
#define MIXIMUS_DECKLINK_API
#endif

namespace miximus::decklink_sdk {

MIXIMUS_DECKLINK_API decklink_ptr<IDeckLinkDiscovery> create_device_discovery();
MIXIMUS_DECKLINK_API std::string get_device_display_name(IDeckLink* device);
MIXIMUS_DECKLINK_API std::string get_display_mode_name(IDeckLinkDisplayMode* mode);
MIXIMUS_DECKLINK_API bool        decklink_iid_equal(REFIID lhs, REFIID rhs) noexcept;
MIXIMUS_DECKLINK_API REFIID      input_video_buffer_iid() noexcept;

template <typename T>
[[nodiscard]] bool decklink_iid_matches(REFIID iid) noexcept
{
    return decklink_iid_equal(iid, decklink_iid<T>());
}

} // namespace miximus::decklink_sdk

#undef MIXIMUS_DECKLINK_API
