#include "media_input_abi.hpp"

namespace miximus::cef_wrapper {

media_frame_s make_media_frame() { return {.size = sizeof(media_frame_s), .fd = -1, .source_generation = 1}; }

} // namespace miximus::cef_wrapper
