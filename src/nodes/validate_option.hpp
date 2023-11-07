#pragma once
#include "gpu/types.hpp"

#include <glm/common.hpp>
#include <nlohmann/json.hpp>

#include <cassert>
#include <optional>

namespace miximus::nodes {

template <typename T>
bool validate_option(nlohmann::json* val, std::optional<T> min = {}, std::optional<T> max = {});

template <typename T>
inline bool validate_number_option(nlohmann::json* val, std::optional<T> min, std::optional<T> max)
{
    if (val == nullptr || !val->is_number()) {
        return false;
    }

    double mmin = min ? *min : std::numeric_limits<T>::min();
    double mmax = max ? *max : std::numeric_limits<T>::max();

    *val = static_cast<T>(glm::clamp(val->get<double>(), mmin, mmax));

    return true;
}

template <>
inline bool validate_option<double>(nlohmann::json* val, std::optional<double> min, std::optional<double> max)
{
    return validate_number_option<double>(val, min, max);
}

template <>
inline bool validate_option<int>(nlohmann::json* val, std::optional<int> min, std::optional<int> max)
{
    return validate_number_option<int>(val, min, max);
}

template <>
inline bool validate_option<bool>(nlohmann::json* val, std::optional<bool> /*min*/, std::optional<bool> /*max*/)
{
    if (val == nullptr || !val->is_boolean()) {
        return false;
    }

    return true;
}

template <>
inline bool
validate_option<gpu::vec2_t>(nlohmann::json* val, std::optional<gpu::vec2_t> min, std::optional<gpu::vec2_t> max)
{
    std::optional<double> minx, maxx, miny, maxy;

    if (val == nullptr || !val->is_array() || val->size() != 2) {
        return false;
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

    return validate_option<gpu::vec2_t::value_type>(&x, minx, maxx) &&
           validate_option<gpu::vec2_t::value_type>(&y, miny, maxy);
}

template <>
inline bool
validate_option<gpu::rect_s>(nlohmann::json* val, std::optional<gpu::rect_s> min, std::optional<gpu::rect_s> max)
{
    std::optional<gpu::vec2_t> minp, maxp, mins, maxs;

    if (val == nullptr || !val->is_object() || val->size() != 2) {
        return false;
    }

    auto pos  = val->find("pos");
    auto size = val->find("size");

    if (pos == val->end() || size == val->end()) {
        return false;
    }

    if (min) {
        minp = min->pos;
        mins = min->size;
    }

    if (max) {
        maxp = max->pos;
        maxs = max->size;
    }

    return validate_option<gpu::vec2_t>(&*pos, minp, maxp) && validate_option<gpu::vec2_t>(&*size, mins, maxs);
}

template <>
inline bool validate_option<std::string>(nlohmann::json* val, std::optional<std::string>, std::optional<std::string>)
{
    return val != nullptr && val->is_string();
}

template <>
inline bool
validate_option<std::string_view>(nlohmann::json* val, std::optional<std::string_view>, std::optional<std::string_view>)
{
    return val != nullptr && val->is_string();
}

} // namespace miximus::nodes
