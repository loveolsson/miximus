#pragma once

#include "output_buffer_limits.hpp"

#include <cstddef>
#include <stdexcept>

namespace miximus {

struct input_buffer_limits_s
{
    static constexpr int MINIMUM_FRAME_COUNT = 1;
    static constexpr int MAXIMUM_FRAME_COUNT = 8;
};
struct decklink_input_buffer_limits_s : input_buffer_limits_s
{
    static constexpr int DEFAULT_FRAME_COUNT = 3;
};
struct ndi_input_buffer_limits_s : input_buffer_limits_s
{
    static constexpr int DEFAULT_FRAME_COUNT = 1;
};
struct cef_capture_buffer_limits_s : input_buffer_limits_s
{
    static constexpr int DEFAULT_FRAME_COUNT = 1;
};
struct cef_export_buffer_limits_s : input_buffer_limits_s
{
    static constexpr int DEFAULT_FRAME_COUNT = 2;
};
struct cef_input_buffer_limits_s : input_buffer_limits_s
{
    // Chromium retains the displayed frame while acquiring its replacement.
    static constexpr int MINIMUM_FRAME_COUNT = 2;
    static constexpr int DEFAULT_FRAME_COUNT = 3;
};

inline size_t validate_input_buffer_frames(size_t frames, size_t minimum = input_buffer_limits_s::MINIMUM_FRAME_COUNT)
{
    if (frames < minimum || frames > input_buffer_limits_s::MAXIMUM_FRAME_COUNT) {
        throw std::invalid_argument("Input buffer depth is outside its supported range");
    }
    return frames;
}

inline constexpr size_t timed_buffer_queue_capacity(size_t target) { return target + 3; }

// Sources retain a current frame outside the queue. Three additional slots
// allow producer, consumer GPU use and asynchronous retirement to overlap.
inline size_t input_buffer_slot_count(size_t target)
{
    return timed_buffer_queue_capacity(validate_input_buffer_frames(target)) + 1 + 3;
}

} // namespace miximus
