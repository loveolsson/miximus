#pragma once
#include "decklink_ptr.hpp"

#include <string>

namespace miximus::decklink_sdk {

decklink_ptr<IDeckLinkDiscovery> create_device_discovery();
std::string                      get_device_display_name(IDeckLink* device);
std::string                      get_display_mode_name(IDeckLinkDisplayMode* mode);
bool                             decklink_iid_equal(REFIID lhs, REFIID rhs) noexcept;
REFIID                           input_video_buffer_iid() noexcept;

template <typename T>
[[nodiscard]] bool decklink_iid_matches(REFIID iid) noexcept
{
    return decklink_iid_equal(iid, decklink_iid<T>());
}

} // namespace miximus::decklink_sdk
