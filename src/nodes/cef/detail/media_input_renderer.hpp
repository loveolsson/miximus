#pragma once

#include "include/cef_frame.h"
#include "include/cef_v8.h"
#include "types/buffer_limits.hpp"

#include <cstdint>
#include <string>

namespace miximus::nodes::cef::detail {

// Installs the page hook only when the runtime advertises the private GPU bridge.
// Must run inside the main document's entered renderer/V8 context.
void                  install_media_inputs(const CefRefPtr<CefFrame>&     frame,
                                           const CefRefPtr<CefV8Context>& context,
                                           const std::string&             token,
                                           uint32_t                       depth = cef_input_buffer_limits_s::DEFAULT_FRAME_COUNT);
inline constexpr auto MEDIA_INPUT_DEPTH_KEY = "miximus.media-input.depth";
inline constexpr auto MEDIA_INPUT_RETIRED   = "miximus.media-input.retired.v1";
inline constexpr auto MEDIA_INPUT_FAILURE   = "miximus.media-input.failure.v1";
inline constexpr auto MEDIA_INPUT_ACTIVITY  = "miximus.media-input.activity.v1";
} // namespace miximus::nodes::cef::detail
