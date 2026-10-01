#include "media_input_abi.hpp"

namespace miximus::cef_wrapper {

media_frame_s make_media_frame()
{
    return {.size = sizeof(media_frame_s), .texture_handle = nullptr, .source_generation = 1};
}

} // namespace miximus::cef_wrapper
