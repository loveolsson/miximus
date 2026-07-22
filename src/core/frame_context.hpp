#pragma once
#include "utils/flicks.hpp"

#include <cstdint>

namespace miximus::core {

struct frame_context_s
{
    uint64_t      frame_number{};
    uint64_t      epoch{};
    uint64_t      settings_revision{};
    utils::flicks pts{};
    utils::flicks duration{};
    utils::flicks target_time{};
    utils::flicks render_deadline{};
    bool          discontinuity{};
};

} // namespace miximus::core
