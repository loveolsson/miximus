#pragma once
#include "utils/flicks.hpp"

#include <cstdint>

namespace miximus::media {

struct media_frame_id_s
{
    uint64_t      epoch{};
    uint64_t      sequence{};
    utils::flicks pts{};
    utils::flicks duration{};
};

} // namespace miximus::media
