#pragma once
#include "wrapper/decklink-sdk/decklink_ptr.hpp"

#include <string>

namespace miximus::nodes::decklink::detail {

decklink_ptr<IDeckLinkDiscovery>       create_device_discovery();
decklink_ptr<IDeckLinkVideoConversion> create_video_conversion();
std::string                            get_device_display_name(IDeckLink* device);
std::string                            get_display_mode_name(IDeckLinkDisplayMode* mode);

} // namespace miximus::nodes::decklink::detail
