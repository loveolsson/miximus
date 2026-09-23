#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>

namespace miximus::nodes::cef::detail {

// Lifetime distribution for one browser generation. Fixed storage and constant
// producer work; caller provides synchronization. Percentiles are bucket upper
// bounds (100 us resolution through 12.8 ms), not exact sample percentiles.
class capture_timing_s
{
    static constexpr uint64_t QUANTUM_US = 100;
    std::array<uint64_t, 129> buckets_{};
    uint64_t                  count_{};
    uint64_t                  maximum_{};

  public:
    struct snapshot_s
    {
        uint64_t samples{};
        uint64_t p50_upper_us{};
        uint64_t p95_upper_us{};
        uint64_t p99_upper_us{};
        uint64_t maximum_us{};
    };

    void add(std::chrono::steady_clock::duration elapsed)
    {
        const auto us =
            static_cast<uint64_t>(std::max<int64_t>(0, std::chrono::ceil<std::chrono::microseconds>(elapsed).count()));
        ++buckets_[std::min<uint64_t>(us / QUANTUM_US, buckets_.size() - 1)];
        ++count_;
        maximum_ = std::max(maximum_, us);
    }

    snapshot_s snapshot() const
    {
        const auto percentile = [&](uint64_t percent) {
            if (count_ == 0)
                return uint64_t{};
            const auto rank = count_ / 100 * percent + (count_ % 100 * percent + 99) / 100;
            uint64_t   cumulative{};
            for (size_t index = 0; index < buckets_.size(); ++index) {
                cumulative += buckets_[index];
                if (cumulative >= rank)
                    return index + 1 == buckets_.size() ? maximum_ : std::min(maximum_, (index + 1) * QUANTUM_US);
            }
            return maximum_;
        };
        return {count_, percentile(50), percentile(95), percentile(99), maximum_};
    }
};
} // namespace miximus::nodes::cef::detail
