#pragma once
#include "utils/flicks.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>

namespace miximus::media {

struct media_frame_id_s
{
    uint64_t      epoch{};
    uint64_t      sequence{};
    utils::flicks pts{};
    utils::flicks duration{};
};

struct source_clock_config_s
{
    size_t        phase_filter_divisor{16};
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
    utils::flicks           offset_{};

  public:
    explicit source_clock_estimator_s(source_clock_config_s config = {});

    source_clock_observation_e   observe(const media_frame_id_s& id, utils::flicks program_observation);
    std::optional<utils::flicks> map(utils::flicks source_pts) const;
    void                         reset();
};

} // namespace miximus::media
