#include "source_clock.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

namespace miximus::media {

source_clock_estimator_s::source_clock_estimator_s(source_clock_config_s config)
    : config_(config)
{
    if (config_.phase_filter_divisor == 0) {
        throw std::invalid_argument("source clock phase filter divisor must be positive");
    }
    if (config_.maximum_phase_adjustment < utils::flicks::zero() ||
        config_.discontinuity_threshold <= utils::flicks::zero()) {
        throw std::invalid_argument("source clock timing limits must be positive");
    }
}

source_clock_observation_e source_clock_estimator_s::observe(const media_frame_id_s& id,
                                                             utils::flicks           program_observation)
{
    const auto initialize = [&] {
        epoch_               = id.epoch;
        sequence_            = id.sequence;
        source_pts_          = id.pts;
        program_observation_ = program_observation;
        offset_              = program_observation - id.pts;
    };

    if (!epoch_.has_value()) {
        initialize();
        return source_clock_observation_e::initialized;
    }

    const auto source_delta  = id.pts - source_pts_;
    const auto program_delta = program_observation - program_observation_;
    const bool discontinuity = id.epoch != *epoch_ || id.sequence <= sequence_ ||
                               source_delta <= utils::flicks::zero() ||
                               std::chrono::abs(program_delta - source_delta) > config_.discontinuity_threshold;
    if (discontinuity) {
        initialize();
        return source_clock_observation_e::discontinuity;
    }

    const auto predicted = id.pts + offset_;
    const auto error     = program_observation - predicted;
    const auto divisor   = static_cast<utils::flicks::rep>(config_.phase_filter_divisor);
    const auto adjustment =
        std::clamp(error / divisor, -config_.maximum_phase_adjustment, config_.maximum_phase_adjustment);
    offset_ += adjustment;
    epoch_               = id.epoch;
    sequence_            = id.sequence;
    source_pts_          = id.pts;
    program_observation_ = program_observation;
    return source_clock_observation_e::updated;
}

std::optional<utils::flicks> source_clock_estimator_s::map(utils::flicks source_pts) const
{
    if (!epoch_.has_value()) {
        return std::nullopt;
    }
    return source_pts + offset_;
}

void source_clock_estimator_s::reset()
{
    epoch_.reset();
    sequence_            = 0;
    source_pts_          = {};
    program_observation_ = {};
    offset_              = {};
}

} // namespace miximus::media
