#pragma once
#include "media/media_clock_sample.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace miximus::media {

struct media_clock_mapping_config_s
{
    size_t        phase_filter_divisor{128};
    size_t        rate_filter_divisor{1};
    size_t        rate_observation_frames{600};
    double        maximum_rate_deviation_ppm{1'000.0};
    utils::flicks maximum_phase_adjustment{utils::to_flicks(0.00025)};
    utils::flicks discontinuity_threshold{utils::to_flicks(0.5)};
};

enum class media_clock_observation_e : uint8_t
{
    initialized,
    updated,
    discontinuity,
};

class media_to_program_clock_s
{
    media_clock_mapping_config_s config_;
    std::optional<uint64_t>      stream_epoch_;
    uint64_t                     frame_sequence_{};
    utils::flicks                media_pts_{};
    utils::flicks                program_time_observation_{};
    utils::flicks                media_anchor_{};
    utils::flicks                program_anchor_{};
    uint64_t                     rate_reference_frame_sequence_{};
    utils::flicks                rate_media_reference_{};
    utils::flicks                rate_program_reference_{};
    long double                  rate_media_sum_{};
    long double                  rate_program_sum_{};
    long double                  rate_media_squared_sum_{};
    long double                  rate_media_program_sum_{};
    size_t                       rate_sample_count_{};
    double                       rate_{1.0};
    std::optional<double>        observed_rate_;
    std::optional<utils::flicks> phase_error_;
    std::optional<utils::flicks> phase_adjustment_;

  public:
    explicit media_to_program_clock_s(media_clock_mapping_config_s config = {});

    media_clock_observation_e    observe(const media_clock_sample_s& sample,
                                         utils::flicks               program_time_observation) noexcept;
    std::optional<utils::flicks> map_media_pts_to_program_time(utils::flicks media_pts) const noexcept;
    std::optional<double>        recovered_rate() const noexcept;
    std::optional<double>        observed_rate() const noexcept;
    std::optional<utils::flicks> phase_offset() const noexcept;
    std::optional<utils::flicks> phase_error() const noexcept;
    std::optional<utils::flicks> phase_adjustment() const noexcept;
    void                         reset() noexcept;
};

} // namespace miximus::media
