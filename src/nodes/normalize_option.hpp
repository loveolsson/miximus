#pragma once
#include "gpu/types.hpp"
#include "option_result.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace miximus::nodes {

template <typename T>
option_result_e normalize_option_value(nlohmann::json* val, std::optional<T> min = {}, std::optional<T> max = {});

template <typename T>
inline option_result_e normalize_number_option_value(nlohmann::json* val, std::optional<T> min, std::optional<T> max)
{
    if (val == nullptr || !val->is_number()) {
        return option_result_e::invalid;
    }

    const double input = val->get<double>();
    if (!std::isfinite(input)) {
        return option_result_e::invalid;
    }

    const double lower = min ? static_cast<double>(*min) : static_cast<double>(std::numeric_limits<T>::lowest());
    const double upper = max ? static_cast<double>(*max) : static_cast<double>(std::numeric_limits<T>::max());
    if (lower > upper) {
        return option_result_e::invalid;
    }

    const T normalized = static_cast<T>(std::clamp(input, lower, upper));
    *val               = normalized;

    return input == static_cast<double>(normalized) ? option_result_e::ok : option_result_e::corrected;
}

template <>
inline option_result_e
normalize_option_value<double>(nlohmann::json* val, std::optional<double> min, std::optional<double> max)
{
    return normalize_number_option_value<double>(val, min, max);
}

template <>
inline option_result_e normalize_option_value<int>(nlohmann::json* val, std::optional<int> min, std::optional<int> max)
{
    return normalize_number_option_value<int>(val, min, max);
}

template <>
inline option_result_e
normalize_option_value<bool>(nlohmann::json* val, std::optional<bool> /*min*/, std::optional<bool> /*max*/)
{
    if (val == nullptr || !val->is_boolean()) {
        return option_result_e::invalid;
    }

    return option_result_e::ok;
}

template <>
inline option_result_e
normalize_option_value<gpu::vec2_t>(nlohmann::json* val, std::optional<gpu::vec2_t> min, std::optional<gpu::vec2_t> max)
{
    std::optional<double> minx, maxx, miny, maxy;

    if (val == nullptr || !val->is_array() || val->size() != 2) {
        return option_result_e::invalid;
    }

    auto& x = val->at(0);
    auto& y = val->at(1);

    if (min) {
        minx = min->x;
        miny = min->y;
    }

    if (max) {
        maxx = max->x;
        maxy = max->y;
    }

    const auto x_result = normalize_option_value<gpu::vec2_t::value_type>(&x, minx, maxx);
    const auto y_result = normalize_option_value<gpu::vec2_t::value_type>(&y, miny, maxy);
    return combine_option_results(x_result, y_result);
}

template <>
inline option_result_e
normalize_option_value<gpu::rect_s>(nlohmann::json* val, std::optional<gpu::rect_s> min, std::optional<gpu::rect_s> max)
{
    std::optional<gpu::vec2_t> minp, maxp, mins, maxs;

    if (val == nullptr || !val->is_object() || val->size() != 2) {
        return option_result_e::invalid;
    }

    auto pos  = val->find("pos");
    auto size = val->find("size");

    if (pos == val->end() || size == val->end()) {
        return option_result_e::invalid;
    }

    if (min) {
        minp = min->pos;
        mins = min->size;
    }

    if (max) {
        maxp = max->pos;
        maxs = max->size;
    }

    const auto pos_result  = normalize_option_value<gpu::vec2_t>(&*pos, minp, maxp);
    const auto size_result = normalize_option_value<gpu::vec2_t>(&*size, mins, maxs);
    return combine_option_results(pos_result, size_result);
}

template <>
inline option_result_e
normalize_option_value<std::string>(nlohmann::json* val, std::optional<std::string>, std::optional<std::string>)
{
    return val != nullptr && val->is_string() ? option_result_e::ok : option_result_e::invalid;
}

template <>
inline option_result_e normalize_option_value<std::string_view>(nlohmann::json* val,
                                                                std::optional<std::string_view>,
                                                                std::optional<std::string_view>)
{
    return val != nullptr && val->is_string() ? option_result_e::ok : option_result_e::invalid;
}

} // namespace miximus::nodes
