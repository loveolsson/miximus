#include "media_clock.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace miximus::media {

media_to_program_clock_s::media_to_program_clock_s(media_clock_mapping_config_s config)
    : config_(config)
{
    if (config_.phase_filter_divisor == 0 || config_.rate_filter_divisor == 0 || config_.rate_observation_frames == 0) {
        throw std::invalid_argument("media clock filter divisors and observation interval must be positive");
    }
    if (config_.maximum_phase_adjustment < utils::flicks::zero() ||
        config_.discontinuity_threshold <= utils::flicks::zero() ||
        !std::isfinite(config_.maximum_rate_deviation_ppm) || config_.maximum_rate_deviation_ppm <= 0.0) {
        throw std::invalid_argument("media clock timing limits must be positive");
    }
}

media_clock_observation_e media_to_program_clock_s::observe(const media_clock_sample_s& sample,
                                                            utils::flicks program_time_observation) noexcept
{
    const auto initialize = [&] {
        stream_epoch_                  = sample.stream_epoch;
        frame_sequence_                = sample.frame_sequence;
        media_pts_                     = sample.media_pts;
        program_time_observation_      = program_time_observation;
        media_anchor_                  = sample.media_pts;
        program_anchor_                = program_time_observation;
        rate_reference_frame_sequence_ = sample.frame_sequence;
        rate_media_reference_          = sample.media_pts;
        rate_program_reference_        = program_time_observation;
        rate_media_sum_                = 0.0L;
        rate_program_sum_              = 0.0L;
        rate_media_squared_sum_        = 0.0L;
        rate_media_program_sum_        = 0.0L;
        rate_sample_count_             = 0;
        rate_                          = 1.0;
        observed_rate_.reset();
        phase_error_.reset();
        phase_adjustment_.reset();
    };

    if (!stream_epoch_.has_value()) {
        initialize();
        return media_clock_observation_e::initialized;
    }

    const auto media_delta   = sample.media_pts - media_pts_;
    const auto program_delta = program_time_observation - program_time_observation_;
    const bool discontinuity = sample.stream_epoch != *stream_epoch_ || sample.frame_sequence <= frame_sequence_ ||
                               media_delta <= utils::flicks::zero() ||
                               std::chrono::abs(program_delta - media_delta) > config_.discontinuity_threshold;
    if (discontinuity) {
        initialize();
        return media_clock_observation_e::discontinuity;
    }

    auto predicted = map_media_pts_to_program_time(sample.media_pts);
    if (!predicted.has_value()) {
        initialize();
        return media_clock_observation_e::discontinuity;
    }

    const auto rate_media_delta   = sample.media_pts - rate_media_reference_;
    const auto rate_program_delta = program_time_observation - rate_program_reference_;
    if (rate_media_delta > utils::flicks::zero() && rate_program_delta > utils::flicks::zero()) {
        const auto media   = static_cast<long double>(rate_media_delta.count());
        const auto program = static_cast<long double>(rate_program_delta.count());
        rate_media_sum_ += media;
        rate_program_sum_ += program;
        rate_media_squared_sum_ += media * media;
        rate_media_program_sum_ += media * program;
        ++rate_sample_count_;
    }

    if (sample.frame_sequence - rate_reference_frame_sequence_ >= config_.rate_observation_frames) {
        const auto sample_count = static_cast<long double>(rate_sample_count_);
        const auto denominator  = (sample_count * rate_media_squared_sum_) - (rate_media_sum_ * rate_media_sum_);
        if (denominator > 0.0L) {
            const auto numerator     = (sample_count * rate_media_program_sum_) - (rate_media_sum_ * rate_program_sum_);
            const auto observed_rate = static_cast<double>(numerator / denominator);
            observed_rate_           = observed_rate;
            const auto maximum_deviation = config_.maximum_rate_deviation_ppm / 1'000'000.0;
            const auto bounded_rate      = std::clamp(observed_rate, 1.0 - maximum_deviation, 1.0 + maximum_deviation);
            media_anchor_                = sample.media_pts;
            program_anchor_              = *predicted;
            rate_ += (bounded_rate - rate_) / static_cast<double>(config_.rate_filter_divisor);
        }
        rate_reference_frame_sequence_ = sample.frame_sequence;
        rate_media_reference_          = sample.media_pts;
        rate_program_reference_        = program_time_observation;
        rate_media_sum_                = 0.0L;
        rate_program_sum_              = 0.0L;
        rate_media_squared_sum_        = 0.0L;
        rate_media_program_sum_        = 0.0L;
        rate_sample_count_             = 0;
    }

    const auto error   = program_time_observation - *predicted;
    const auto divisor = static_cast<utils::flicks::rep>(config_.phase_filter_divisor);
    const auto adjustment =
        std::clamp(error / divisor, -config_.maximum_phase_adjustment, config_.maximum_phase_adjustment);
    phase_error_      = error;
    phase_adjustment_ = adjustment;
    program_anchor_ += adjustment;
    stream_epoch_             = sample.stream_epoch;
    frame_sequence_           = sample.frame_sequence;
    media_pts_                = sample.media_pts;
    program_time_observation_ = program_time_observation;
    return media_clock_observation_e::updated;
}

std::optional<utils::flicks>
media_to_program_clock_s::map_media_pts_to_program_time(utils::flicks media_pts) const noexcept
{
    if (!stream_epoch_.has_value()) {
        return std::nullopt;
    }
    const auto media_delta =
        static_cast<long double>(media_pts.count()) - static_cast<long double>(media_anchor_.count());
    const auto mapped_count =
        static_cast<long double>(program_anchor_.count()) + std::round(media_delta * static_cast<long double>(rate_));
    if (mapped_count < static_cast<long double>(std::numeric_limits<utils::flicks::rep>::min()) ||
        mapped_count > static_cast<long double>(std::numeric_limits<utils::flicks::rep>::max())) {
        return std::nullopt;
    }
    return utils::flicks{static_cast<utils::flicks::rep>(mapped_count)};
}

std::optional<double> media_to_program_clock_s::recovered_rate() const noexcept
{
    return stream_epoch_.has_value() ? std::optional(rate_) : std::nullopt;
}

std::optional<double> media_to_program_clock_s::observed_rate() const noexcept { return observed_rate_; }

std::optional<utils::flicks> media_to_program_clock_s::phase_offset() const noexcept
{
    const auto mapped = map_media_pts_to_program_time(media_pts_);
    return mapped.has_value() ? std::optional(*mapped - media_pts_) : std::nullopt;
}

std::optional<utils::flicks> media_to_program_clock_s::phase_error() const noexcept { return phase_error_; }

std::optional<utils::flicks> media_to_program_clock_s::phase_adjustment() const noexcept { return phase_adjustment_; }

void media_to_program_clock_s::reset() noexcept
{
    stream_epoch_.reset();
    frame_sequence_                = 0;
    media_pts_                     = {};
    program_time_observation_      = {};
    media_anchor_                  = {};
    program_anchor_                = {};
    rate_reference_frame_sequence_ = 0;
    rate_media_reference_          = {};
    rate_program_reference_        = {};
    rate_media_sum_                = 0.0L;
    rate_program_sum_              = 0.0L;
    rate_media_squared_sum_        = 0.0L;
    rate_media_program_sum_        = 0.0L;
    rate_sample_count_             = 0;
    rate_                          = 1.0;
    observed_rate_.reset();
    phase_error_.reset();
    phase_adjustment_.reset();
}

} // namespace miximus::media
