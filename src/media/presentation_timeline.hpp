#pragma once
#include "utils/flicks.hpp"

#include <array>
#include <cstddef>
#include <optional>

namespace miximus::media {

class presentation_timeline_s
{
    static constexpr size_t LATENCY_OBSERVATION_COUNT = 16;

    std::array<utils::flicks, LATENCY_OBSERVATION_COUNT> latency_observations_{};
    size_t                                               latency_observation_count_{};
    size_t                                               next_latency_observation_{};
    long double                                          latency_sum_{};
    std::optional<utils::flicks>                         latency_;

  public:
    utils::flicks observe_latency(utils::flicks presentation_time, utils::flicks program_target_time) noexcept
    {
        const auto observation = presentation_time - program_target_time;
        if (latency_observation_count_ == LATENCY_OBSERVATION_COUNT) {
            latency_sum_ -= static_cast<long double>(latency_observations_[next_latency_observation_].count());
        } else {
            ++latency_observation_count_;
        }
        latency_observations_[next_latency_observation_] = observation;
        next_latency_observation_                        = (next_latency_observation_ + 1) % LATENCY_OBSERVATION_COUNT;
        latency_sum_ += static_cast<long double>(observation.count());

        latency_ = utils::flicks{
            static_cast<utils::flicks::rep>(latency_sum_ / static_cast<long double>(latency_observation_count_))};
        return *latency_;
    }

    std::optional<utils::flicks> map_presentation_to_program_target(utils::flicks presentation_time) const noexcept
    {
        if (!latency_.has_value()) {
            return std::nullopt;
        }
        return presentation_time - *latency_;
    }

    std::optional<utils::flicks> latency() const noexcept { return latency_; }
};

} // namespace miximus::media
