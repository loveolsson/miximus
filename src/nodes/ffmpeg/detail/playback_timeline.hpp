#pragma once

#include "utils/flicks.hpp"

#include <cstdint>
#include <optional>

namespace miximus::nodes::ffmpeg::detail {

// Render-thread transport. Decoder throughput never participates in this clock.
class playback_timeline_s
{
    utils::flicks           position_{};
    utils::flicks           previous_pts_{};
    std::optional<uint64_t> epoch_;
    bool                    running_{};

  public:
    utils::flicks position() const noexcept { return position_; }

    utils::flicks preview(utils::flicks pts, uint64_t epoch) const noexcept
    {
        return position_ +
               (running_ && epoch_ == epoch && pts >= previous_pts_ ? pts - previous_pts_ : utils::flicks{});
    }

    void tick(utils::flicks pts, uint64_t epoch, bool active) noexcept
    {
        if (active) {
            position_ = preview(pts, epoch);
        }

        previous_pts_ = pts;
        epoch_        = epoch;
        running_      = active;
    }

    void rewind() noexcept
    {
        position_ = {};
        running_  = false;
    }
};

} // namespace miximus::nodes::ffmpeg::detail
