#pragma once
#include "gpu/types.hpp"
#include "types/frame_rate.hpp"

namespace miximus::nodes {
struct frame_settings_s
{
    struct decklink_input_settings_s
    {
        decklink_input_settings_s();
        int buffer_frames;
    };

    decklink_input_settings_s decklink_input;

    struct ndi_input_settings_s
    {
        ndi_input_settings_s();
        int buffer_frames;
    };

    ndi_input_settings_s ndi_input;

    struct ffmpeg_settings_s
    {
        int buffer_frames{4};
    };

    ffmpeg_settings_s ffmpeg;

    struct cef_capture_settings_s
    {
        cef_capture_settings_s();
        int buffer_frames;
    };

    cef_capture_settings_s cef_capture;

    struct cef_export_settings_s
    {
        cef_export_settings_s();
        int buffer_frames;
    };

    cef_export_settings_s cef_export;

    struct cef_input_settings_s
    {
        cef_input_settings_s();
        int buffer_frames;
    };

    cef_input_settings_s cef_input;

    struct decklink_output_settings_s
    {
        decklink_output_settings_s();
        int buffer_frames;
    };

    struct ndi_output_settings_s
    {
        ndi_output_settings_s();
        int buffer_frames;
    };

    struct screen_output_settings_s
    {
        screen_output_settings_s();
        int buffer_frames;
    };

    struct framebuffer_settings_s
    {
        static constexpr int DEFAULT_WIDTH  = 1920;
        static constexpr int DEFAULT_HEIGHT = 1080;
        static constexpr int MIN_DIMENSION  = 256;
        static constexpr int MAX_DIMENSION  = 4096;

        gpu::vec2i_t default_size{DEFAULT_WIDTH, DEFAULT_HEIGHT};
    };

    frame_rate_s               frame_rate{DEFAULT_FRAME_RATE};
    framebuffer_settings_s     framebuffer;
    decklink_output_settings_s decklink_output;
    ndi_output_settings_s      ndi_output;
    screen_output_settings_s   screen_output;
};
} // namespace miximus::nodes
