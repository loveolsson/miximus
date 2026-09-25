#pragma once

#include "gpu/recording.hpp"

#include <array>
#include <filesystem>
#include <memory>

namespace miximus::gpu::detail {

// Hardware-test helper only. Compare every pixel on GPU against a uniform
// reference over an optional horizontal range. The host reads two aggregate error counters, never image pixels.
class color_comparison_s
{
    struct state_s;
    std::shared_ptr<state_s> state_;

  public:
    color_comparison_s(const texture_s& source, const std::filesystem::path& shader);

    ~color_comparison_s();
    void record(recording_s&         recording,
                const texture_s&     source,
                const buffer_s&      counters,
                std::array<float, 4> reference,
                float                tolerance,
                uint32_t             x_begin = 0,
                uint32_t             x_end   = 0);
};

} // namespace miximus::gpu::detail
