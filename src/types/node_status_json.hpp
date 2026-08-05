#pragma once
#include "json_contract.hpp"
#include "node_status.hpp"

#include <nlohmann/json.hpp>

#include <chrono>

namespace nlohmann {

template <>
struct adl_serializer<miximus::utils::flicks>
{
    static void to_json(json& value, const miximus::utils::flicks& duration)
    {
        value = std::chrono::duration_cast<std::chrono::microseconds>(duration).count();
    }
};

} // namespace nlohmann

namespace miximus::status {

template <described_json_object T>
void to_json(nlohmann::json& json, const T& value)
{
    detail::write_described_json(json, value);
}

} // namespace miximus::status
