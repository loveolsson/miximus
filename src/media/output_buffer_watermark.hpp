#pragma once

#include <cstddef>
#include <stdexcept>

namespace miximus::media {

class output_buffer_watermark_s
{
    size_t target_{};
    size_t scheduled_{};

  public:
    explicit output_buffer_watermark_s(size_t target)
        : target_(target)
    {
        if (target_ == 0) {
            throw std::invalid_argument("output buffer target must be positive");
        }
    }

    void frame_scheduled() { ++scheduled_; }

    bool frame_completed()
    {
        if (scheduled_ == 0) {
            return false;
        }
        --scheduled_;
        return true;
    }

    size_t refill_count() const { return target_ > scheduled_ ? target_ - scheduled_ : 0; }
    size_t scheduled_frames() const { return scheduled_; }
    size_t target() const { return target_; }
};

} // namespace miximus::media
