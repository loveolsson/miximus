#pragma once

#include "include/cef_frame.h"
#include "include/cef_v8.h"

#include <string>

namespace miximus::nodes::cef::detail {
// Installs the page hook only when the runtime advertises the private GPU bridge.
// Must run inside the main document's entered renderer/V8 context.
void                  install_media_inputs(const CefRefPtr<CefFrame>&     frame,
                                           const CefRefPtr<CefV8Context>& context,
                                           const std::string&             token);
inline constexpr auto MEDIA_INPUT_SUBSCRIBE = "miximus.media-input.subscribe.v1";
} // namespace miximus::nodes::cef::detail
