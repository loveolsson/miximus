#pragma once

#include "gpu/detail/external_image.hpp"
#include "include/cef_render_handler.h"
#include "wrapper/cef/media_input_abi.hpp"

namespace miximus::nodes::cef::detail {

gpu::detail::external_image_s capture_image(const CefAcceleratedPaintInfo& info);
void                          set_media_frame_image(cef_wrapper::media_frame_s&          packet,
                                                    const gpu::detail::external_image_s& image,
                                                    size_t                               bytes);

} // namespace miximus::nodes::cef::detail
