#pragma once
#include "utils/flicks.hpp"

#include <optional>

namespace miximus::media {

class output_timeline_s
{
    std::optional<utils::flicks> latency_;

  public:
    utils::flicks align(utils::flicks presentation_time, utils::flicks program_target_time) noexcept
    {
        const auto latency = presentation_time - program_target_time;
        latency_           = latency;
        return latency;
    }

    std::optional<utils::flicks> program_target_time(utils::flicks presentation_time) const noexcept
    {
        if (!latency_.has_value()) {
            return std::nullopt;
        }
        return presentation_time - *latency_;
    }

    std::optional<utils::flicks> latency() const noexcept { return latency_; }
};

} // namespace miximus::media
