#pragma once
#include "media/media_frame.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace miximus::media {

struct source_clock_config_s
{
    size_t        phase_filter_divisor{16};
    size_t        rate_filter_divisor{4};
    size_t        rate_observation_frames{32};
    double        maximum_rate_deviation_ppm{5'000.0};
    utils::flicks maximum_phase_adjustment{utils::to_flicks(0.001)};
    utils::flicks discontinuity_threshold{utils::to_flicks(0.5)};
};

enum class source_clock_observation_e : uint8_t
{
    initialized,
    updated,
    discontinuity,
};

class source_clock_estimator_s
{
    source_clock_config_s   config_;
    std::optional<uint64_t> epoch_;
    uint64_t                sequence_{};
    utils::flicks           source_pts_{};
    utils::flicks           program_observation_{};
    utils::flicks           source_anchor_{};
    utils::flicks           program_anchor_{};
    uint64_t                rate_reference_sequence_{};
    utils::flicks           rate_source_reference_{};
    utils::flicks           rate_program_reference_{};
    double                  rate_{1.0};

  public:
    explicit source_clock_estimator_s(source_clock_config_s config = {});

    source_clock_observation_e   observe(const media_frame_id_s& id, utils::flicks program_observation);
    std::optional<utils::flicks> map(utils::flicks source_pts) const;
    std::optional<double>        recovered_rate() const;
    std::optional<utils::flicks> phase_offset() const;
    void                         reset();
};

} // namespace miximus::media
