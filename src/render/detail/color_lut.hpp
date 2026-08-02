#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace miximus::render::detail {

template <size_t Root>
constexpr double nth_root(double value) noexcept
{
    static_assert(Root >= 2);
    if (value <= 0.0) {
        return 0.0;
    }

    double estimate = 1.0;
    for (size_t iteration = 0; iteration < 96; ++iteration) {
        double denominator = 1.0;
        for (size_t exponent = 1; exponent < Root; ++exponent) {
            denominator *= estimate;
        }
        estimate = ((static_cast<double>(Root - 1) * estimate) + (value / denominator)) / static_cast<double>(Root);
    }
    return estimate;
}

constexpr double srgb_to_linear(double encoded) noexcept
{
    if (encoded <= 0.04045) {
        return encoded / 12.92;
    }

    const double base    = (encoded + 0.055) / 1.055;
    const double squared = base * base;
    return squared * nth_root<5>(squared);
}

constexpr double rec709_to_linear(double encoded) noexcept
{
    if (encoded < 0.081) {
        return encoded / 4.5;
    }

    const double base    = (encoded + 0.099) / 1.099;
    const double squared = base * base;
    return squared * nth_root<9>(squared);
}

constexpr uint8_t normalized_to_u8(double value) noexcept
{
    const double clamped = value < 0.0 ? 0.0 : value > 1.0 ? 1.0 : value;
    return static_cast<uint8_t>((clamped * 255.0) + 0.5);
}

template <typename Transfer>
consteval std::array<uint8_t, 256> make_u8_lut(Transfer transfer)
{
    std::array<uint8_t, 256> result{};
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = normalized_to_u8(transfer(static_cast<double>(i)));
    }
    return result;
}

inline constexpr auto SRGB_TO_LINEAR_U8 =
    make_u8_lut([](double value) constexpr { return srgb_to_linear(value / 255.0); });

inline constexpr auto VIDEO_REC709_TO_LINEAR_U8 = make_u8_lut([](double value) constexpr {
    constexpr double BLACK   = 16.0;
    constexpr double RANGE   = 219.0;
    const double     encoded = (value - BLACK) / RANGE;
    const double     clamped = encoded < 0.0 ? 0.0 : encoded > 1.0 ? 1.0 : encoded;
    return rec709_to_linear(clamped);
});

static_assert(SRGB_TO_LINEAR_U8.front() == 0);
static_assert(SRGB_TO_LINEAR_U8[128] == 55);
static_assert(SRGB_TO_LINEAR_U8.back() == 255);

} // namespace miximus::render::detail
