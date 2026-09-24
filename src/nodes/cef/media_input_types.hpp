#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
namespace miximus::nodes::cef {
inline constexpr std::array<std::string_view, 8>
    MEDIA_INPUT_NAMES{"input_0", "input_1", "input_2", "input_3", "input_4", "input_5", "input_6", "input_7"};
struct media_input_metrics_s
{
    bool        available{}, failed{};
    uint32_t    subscribed{};
    uint64_t    submitted{}, delivered{}, drops{}, occupied{}, reserved_bytes{};
    std::string error;
};
namespace detail {
class media_input_runtime_s;
}
} // namespace miximus::nodes::cef
