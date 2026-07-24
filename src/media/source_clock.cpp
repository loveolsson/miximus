#include "source_clock.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace miximus::media {

source_clock_estimator_s::source_clock_estimator_s(source_clock_config_s config)
    : config_(config)
{
    if (config_.phase_filter_divisor == 0 || config_.rate_filter_divisor == 0 || config_.rate_observation_frames == 0) {
        throw std::invalid_argument("source clock filter divisors and observation interval must be positive");
    }
    if (config_.maximum_phase_adjustment < utils::flicks::zero() ||
        config_.discontinuity_threshold <= utils::flicks::zero() ||
        !std::isfinite(config_.maximum_rate_deviation_ppm) || config_.maximum_rate_deviation_ppm <= 0.0) {
        throw std::invalid_argument("source clock timing limits must be positive");
    }
}

source_clock_observation_e source_clock_estimator_s::observe(const media_frame_id_s& id,
                                                             utils::flicks           program_observation)
{
    const auto initialize = [&] {
        epoch_                   = id.epoch;
        sequence_                = id.sequence;
        source_pts_              = id.pts;
        program_observation_     = program_observation;
        source_anchor_           = id.pts;
        program_anchor_          = program_observation;
        rate_reference_sequence_ = id.sequence;
        rate_source_reference_   = id.pts;
        rate_program_reference_  = program_observation;
        rate_source_sum_         = 0.0L;
        rate_program_sum_        = 0.0L;
        rate_source_squared_sum_ = 0.0L;
        rate_source_program_sum_ = 0.0L;
        rate_sample_count_       = 0;
        rate_                    = 1.0;
        observed_rate_.reset();
        phase_error_.reset();
        phase_adjustment_.reset();
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

    auto predicted = map(id.pts);
    if (!predicted.has_value()) {
        initialize();
        return source_clock_observation_e::discontinuity;
    }

    const auto rate_source_delta  = id.pts - rate_source_reference_;
    const auto rate_program_delta = program_observation - rate_program_reference_;
    if (rate_source_delta > utils::flicks::zero() && rate_program_delta > utils::flicks::zero()) {
        const auto source  = static_cast<long double>(rate_source_delta.count());
        const auto program = static_cast<long double>(rate_program_delta.count());
        rate_source_sum_ += source;
        rate_program_sum_ += program;
        rate_source_squared_sum_ += source * source;
        rate_source_program_sum_ += source * program;
        ++rate_sample_count_;
    }

    if (id.sequence - rate_reference_sequence_ >= config_.rate_observation_frames) {
        const auto sample_count = static_cast<long double>(rate_sample_count_);
        const auto denominator  = sample_count * rate_source_squared_sum_ - rate_source_sum_ * rate_source_sum_;
        if (denominator > 0.0L) {
            const auto numerator     = sample_count * rate_source_program_sum_ - rate_source_sum_ * rate_program_sum_;
            const auto observed_rate = static_cast<double>(numerator / denominator);
            observed_rate_           = observed_rate;
            const auto maximum_deviation = config_.maximum_rate_deviation_ppm / 1'000'000.0;
            const auto bounded_rate      = std::clamp(observed_rate, 1.0 - maximum_deviation, 1.0 + maximum_deviation);
            source_anchor_               = id.pts;
            program_anchor_              = *predicted;
            rate_ += (bounded_rate - rate_) / static_cast<double>(config_.rate_filter_divisor);
        }
        rate_reference_sequence_ = id.sequence;
        rate_source_reference_   = id.pts;
        rate_program_reference_  = program_observation;
        rate_source_sum_         = 0.0L;
        rate_program_sum_        = 0.0L;
        rate_source_squared_sum_ = 0.0L;
        rate_source_program_sum_ = 0.0L;
        rate_sample_count_       = 0;
    }

    const auto error   = program_observation - *predicted;
    const auto divisor = static_cast<utils::flicks::rep>(config_.phase_filter_divisor);
    const auto adjustment =
        std::clamp(error / divisor, -config_.maximum_phase_adjustment, config_.maximum_phase_adjustment);
    phase_error_      = error;
    phase_adjustment_ = adjustment;
    program_anchor_ += adjustment;
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
    const auto source_delta =
        static_cast<long double>(source_pts.count()) - static_cast<long double>(source_anchor_.count());
    const auto mapped_count =
        static_cast<long double>(program_anchor_.count()) + std::round(source_delta * static_cast<long double>(rate_));
    if (mapped_count < static_cast<long double>(std::numeric_limits<utils::flicks::rep>::min()) ||
        mapped_count > static_cast<long double>(std::numeric_limits<utils::flicks::rep>::max())) {
        return std::nullopt;
    }
    return utils::flicks{static_cast<utils::flicks::rep>(mapped_count)};
}

std::optional<double> source_clock_estimator_s::recovered_rate() const
{
    return epoch_.has_value() ? std::optional(rate_) : std::nullopt;
}

std::optional<double> source_clock_estimator_s::observed_rate() const { return observed_rate_; }

std::optional<utils::flicks> source_clock_estimator_s::phase_offset() const
{
    const auto mapped = map(source_pts_);
    return mapped.has_value() ? std::optional(*mapped - source_pts_) : std::nullopt;
}

std::optional<utils::flicks> source_clock_estimator_s::phase_error() const { return phase_error_; }

std::optional<utils::flicks> source_clock_estimator_s::phase_adjustment() const { return phase_adjustment_; }

void source_clock_estimator_s::reset()
{
    epoch_.reset();
    sequence_                = 0;
    source_pts_              = {};
    program_observation_     = {};
    source_anchor_           = {};
    program_anchor_          = {};
    rate_reference_sequence_ = 0;
    rate_source_reference_   = {};
    rate_program_reference_  = {};
    rate_source_sum_         = 0.0L;
    rate_program_sum_        = 0.0L;
    rate_source_squared_sum_ = 0.0L;
    rate_source_program_sum_ = 0.0L;
    rate_sample_count_       = 0;
    rate_                    = 1.0;
    observed_rate_.reset();
    phase_error_.reset();
    phase_adjustment_.reset();
}

} // namespace miximus::media
