#pragma once

#include <nlohmann/json.hpp>

#include <string_view>

namespace miximus::nodes::math {

enum class operation_e
{
    add,
    sub,
    mul,
    min,
    max,
};

constexpr operation_e op_from_string(std::string_view topic)
{
    if (topic == "sub") {
        return operation_e::sub;
    }

    if (topic == "mul") {
        return operation_e::mul;
    }

    if (topic == "min") {
        return operation_e::min;
    }

    if (topic == "max") {
        return operation_e::max;
    }

    return operation_e::add;
}

constexpr std::string_view op_to_string(operation_e topic)
{
    switch (topic) {
        case operation_e::add:
            return "add";
        case operation_e::sub:
            return "sub";
        case operation_e::mul:
            return "mul";
        case operation_e::min:
            return "min";
        case operation_e::max:
            return "max";
        default:
            return "add";
    }
}

constexpr auto operation_setter = [](operation_e& t, const nlohmann::json& j) -> bool {
    try {
        t = op_from_string(j.get<std::string_view>());
    } catch (nlohmann::json::exception& e) {
        return false;
    }

    return true;
};

constexpr auto operation_getter = [](const operation_e& t) -> nlohmann::json { return op_to_string(t); };

} // namespace miximus::nodes::math