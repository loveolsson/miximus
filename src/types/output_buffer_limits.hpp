#pragma once

namespace miximus {

struct decklink_output_buffer_limits_s
{
    static constexpr int DEFAULT_FRAME_COUNT = 4;
    static constexpr int MINIMUM_FRAME_COUNT = 1;
    static constexpr int MAXIMUM_FRAME_COUNT = 8;
};

struct ndi_output_buffer_limits_s
{
    static constexpr int DEFAULT_FRAME_COUNT = 4;
    static constexpr int MINIMUM_FRAME_COUNT = 1;
    static constexpr int MAXIMUM_FRAME_COUNT = 8;
};

struct screen_output_buffer_limits_s
{
    static constexpr int DEFAULT_FRAME_COUNT = 2;
    static constexpr int MINIMUM_FRAME_COUNT = 1;
    static constexpr int MAXIMUM_FRAME_COUNT = 8;
};

} // namespace miximus
