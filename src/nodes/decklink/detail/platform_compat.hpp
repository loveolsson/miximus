#pragma once
#include "wrapper/decklink-sdk/decklink_ptr.hpp"

#include <string>

namespace miximus::nodes::decklink::detail {

decklink_ptr<IDeckLinkDiscovery>       create_device_discovery();
decklink_ptr<IDeckLinkVideoConversion> create_video_conversion();
std::string                            get_device_display_name(IDeckLink* device);
std::string                            get_display_mode_name(IDeckLinkDisplayMode* mode);
bool                                   decklink_iid_equal(REFIID lhs, REFIID rhs) noexcept;
REFIID                                 upload_video_buffer_iid() noexcept;

} // namespace miximus::nodes::decklink::detail

namespace miximus::nodes::decklink {

template <typename T>
[[nodiscard]] bool decklink_iid_matches(REFIID iid) noexcept
{
    return detail::decklink_iid_equal(iid, decklink_iid<T>());
}

} // namespace miximus::nodes::decklink
