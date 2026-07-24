#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace miximus::media {

// Samples bytes across the complete frame. This is intended for cadence
// diagnostics, not content identity or integrity checks.
inline uint64_t sampled_frame_fingerprint(std::span<const std::byte> bytes)
{
    constexpr size_t SAMPLE_COUNT = 8'192;
    constexpr uint64_t FNV_OFFSET = 14'695'981'039'346'656'037ULL;
    constexpr uint64_t FNV_PRIME  = 1'099'511'628'211ULL;

    uint64_t fingerprint = FNV_OFFSET;
    const auto samples    = std::min(bytes.size(), SAMPLE_COUNT);
    for (size_t sample = 0; sample < samples; ++sample) {
        const auto index = sample * bytes.size() / samples;
        fingerprint ^= std::to_integer<uint8_t>(bytes[index]);
        fingerprint *= FNV_PRIME;
    }
    fingerprint ^= bytes.size();
    fingerprint *= FNV_PRIME;
    return fingerprint;
}

} // namespace miximus::media
