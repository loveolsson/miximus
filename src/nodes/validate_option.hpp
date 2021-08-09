#pragma once
#include "gpu/types.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes::detail {

template <typename T>
bool validate_option(const nlohmann::json&);

template <>
inline bool validate_option<bool>(const nlohmann::json& val)
{
    return val.is_boolean();
}

template <>
inline bool validate_option<double>(const nlohmann::json& val)
{
    return val.is_number();
}

template <>
inline bool validate_option<gpu::vec2_t>(const nlohmann::json& val)
{
    if (!val.is_array() || val.size() != 2) {
        return false;
    }
    if (!val[0].is_number() || !val[1].is_number()) {
        return false;
    }
    return true;
}

template <>
inline bool validate_option<gpu::rect_s>(const nlohmann::json& val)
{
    return true;
}

template <>
inline bool validate_option<std::string>(const nlohmann::json& val)
{
    return val.is_string();
}

template <>
inline bool validate_option<std::string_view>(const nlohmann::json& val)
{
    return val.is_string();
}

} // namespace miximus::nodes::detail
