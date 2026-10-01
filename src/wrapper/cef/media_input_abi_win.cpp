#include "media_input_abi.hpp"

namespace miximus::cef_wrapper {

media_frame_s make_media_frame()
{
    media_frame_s frame{};
    frame.size              = sizeof(media_frame_s);
    frame.source_generation = 1;
    return frame;
}

} // namespace miximus::cef_wrapper
