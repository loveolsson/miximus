#include "frame_settings.hpp"

#include "types/buffer_limits.hpp"

namespace miximus::nodes {

frame_settings_s::decklink_input_settings_s::decklink_input_settings_s()
    : buffer_frames(decklink_input_buffer_limits_s::DEFAULT_FRAME_COUNT)
{
}

frame_settings_s::ndi_input_settings_s::ndi_input_settings_s()
    : buffer_frames(ndi_input_buffer_limits_s::DEFAULT_FRAME_COUNT)
{
}

frame_settings_s::cef_capture_settings_s::cef_capture_settings_s()
    : buffer_frames(cef_capture_buffer_limits_s::DEFAULT_FRAME_COUNT)
{
}

frame_settings_s::cef_export_settings_s::cef_export_settings_s()
    : buffer_frames(cef_export_buffer_limits_s::DEFAULT_FRAME_COUNT)
{
}

frame_settings_s::cef_input_settings_s::cef_input_settings_s()
    : buffer_frames(cef_input_buffer_limits_s::DEFAULT_FRAME_COUNT)
{
}

frame_settings_s::decklink_output_settings_s::decklink_output_settings_s()
    : buffer_frames(decklink_output_buffer_limits_s::DEFAULT_FRAME_COUNT)
{
}

frame_settings_s::ndi_output_settings_s::ndi_output_settings_s()
    : buffer_frames(ndi_output_buffer_limits_s::DEFAULT_FRAME_COUNT)
{
}

frame_settings_s::screen_output_settings_s::screen_output_settings_s()
    : buffer_frames(screen_output_buffer_limits_s::DEFAULT_FRAME_COUNT)
{
}

} // namespace miximus::nodes
