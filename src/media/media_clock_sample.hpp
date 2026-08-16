#pragma once
#include "utils/flicks.hpp"

#include <cstdint>

namespace miximus::media {

struct media_clock_sample_s
{
    uint64_t      stream_epoch{};
    uint64_t      frame_sequence{};
    utils::flicks media_pts{};
    utils::flicks frame_duration{};
};

} // namespace miximus::media
