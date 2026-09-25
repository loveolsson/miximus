#pragma once

#include "include/internal/cef_miximus_media_input.h"

namespace miximus::cef_wrapper {

// The packaged SDK owns the private ABI definition; only symbol lookup stays here.
using media_frame_s          = cef_miximus_media_frame_t;
using media_done_t           = cef_miximus_media_done_t;
using install_media_inputs_t = decltype(&cef_miximus_install_media_inputs_v1);
using send_media_frame_t     = decltype(&cef_miximus_send_media_frame_v2);

inline media_frame_s make_media_frame()
{
    media_frame_s frame{};
    frame.size              = sizeof(frame);
    frame.fd                = -1;
    frame.source_generation = 1;
    return frame;
}

inline constexpr auto INSTALL_MEDIA_INPUTS = "cef_miximus_install_media_inputs_v1";
inline constexpr auto SEND_MEDIA_FRAME     = "cef_miximus_send_media_frame_v2";

} // namespace miximus::cef_wrapper
