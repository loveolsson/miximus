#pragma once
#include "gpu/types.hpp"

#include <glm/common.hpp>
#include <nlohmann/json.hpp>

#include <cassert>
#include <optional>

namespace miximus::nodes {

template <typename T>
bool validate_option(nlohmann::json* val, std::optional<T> min = std::nullopt, std::optional<T> max = std::nullopt);

template <>
inline bool validate_option<double>(nlohmann::json* val, std::optional<double> min, std::optional<double> max)
{
    if (val == nullptr) {
        return false;
    }

    if (!val->is_number()) {
        return false;
    }

    if (min && *val < *min) {
        *val = *min;
    }

    if (max && *val > *max) {
        *val = *max;
    }

    return true;
}

template <>
inline bool validate_option<bool>(nlohmann::json* val, std::optional<bool> /*min*/, std::optional<bool> /*max*/)
{
    if (val == nullptr) {
        return false;
    }

    if (!val->is_boolean()) {
        return false;
    }

    return true;
}

template <>
inline bool validate_option<int>(nlohmann::json* val, std::optional<int> min, std::optional<int> max)
{
    if (val == nullptr) {
        return false;
    }

    if (!val->is_number()) {
        return false;
    }

    if (!val->is_number_integer()) {
        *val = glm::floor(val->get<double>());
        assert(val->is_number_integer());
    }

    if (min && *val < *min) {
        *val = *min;
    }

    if (max && *val > *max) {
        *val = *max;
    }

    return true;
}

template <>
inline bool
validate_option<gpu::vec2_t>(nlohmann::json* val, std::optional<gpu::vec2_t> min, std::optional<gpu::vec2_t> max)
{
    std::optional<double> minx, maxx, miny, maxy;

    if (val == nullptr) {
        return false;
    }

    if (!val->is_array() || val->size() != 2) {
        return false;
    }

    auto& x = val->at(0);
    auto& y = val->at(1);

    if (!x.is_number() || !y.is_number()) {
        return false;
    }

    if (min) {
        minx = min->x;
        miny = min->y;
    }

    if (max) {
        maxx = max->x;
        maxy = max->y;
    }

    return validate_option<double>(&x, minx, maxx) && validate_option<double>(&y, miny, maxy);
}

template <>
inline bool
validate_option<gpu::rect_s>(nlohmann::json* val, std::optional<gpu::rect_s> min, std::optional<gpu::rect_s> max)
{
    std::optional<gpu::vec2_t> minp, maxp, mins, maxs;

    if (val == nullptr) {
        return false;
    }

    if (!val->is_object() || val->size() != 2) {
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
    if (val == nullptr) {
        return false;
    }

    return val->is_string();
}

template <>
inline bool
validate_option<std::string_view>(nlohmann::json* val, std::optional<std::string_view>, std::optional<std::string_view>)
{
    if (val == nullptr) {
        return false;
    }

    return val->is_string();
}

} // namespace miximus::nodes
