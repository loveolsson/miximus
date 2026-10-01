#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace miximus::render::detail {

// Local to an image generation. Invert the Rec.709 transfer function at each
// 8-bit rounding boundary, using the float formula from shaders/common.glsl.
class rec709_quantizer_s
{
    std::array<float, 255>   cutoffs_{};
    std::array<uint8_t, 256> first_values_{};

  public:
    rec709_quantizer_s()
    {
        for (size_t i = 0; i < cutoffs_.size(); ++i) {
            const float linear = (static_cast<float>(i) + 0.5F) / 255.0F;
            cutoffs_[i]        = linear < 0.018F ? linear * 4.5F : (1.099F * std::pow(linear, 0.45F)) - 0.099F;
        }
        for (size_t i = 0; i < first_values_.size(); ++i) {
            const float lower = static_cast<float>(i) / first_values_.size();
            first_values_[i] =
                static_cast<uint8_t>(std::upper_bound(cutoffs_.begin(), cutoffs_.end(), lower) - cutoffs_.begin());
        }
    }

    [[nodiscard]] uint8_t operator()(float encoded) const noexcept
    {
        // The coarse index only skips impossible results; comparisons against
        // the continuous input still decide the output at each cutoff.
        encoded           = std::clamp(encoded, 0.0F, 1.0F);
        const auto bucket = std::min(static_cast<size_t>(encoded * first_values_.size()), first_values_.size() - 1);
        size_t     value  = first_values_[bucket];
        while (value < cutoffs_.size() && encoded >= cutoffs_[value]) {
            ++value;
        }
        return static_cast<uint8_t>(value);
    }
};

} // namespace miximus::render::detail
