#pragma once
#include <frozen/map.h>
#include <nlohmann/json.hpp>

namespace miximus::nodes::math {

enum class operation
{
    add = 0,
    sub,
    mul,
    min,
    max,
    _count,
};

constexpr frozen::map<std::string_view, operation, (size_t)operation::_count> op_lookup_table = {
    {"add", operation::add},
    {"subtract", operation::sub},
    {"multiply", operation::mul},
    {"min", operation::min},
    {"max", operation::max},
};

constexpr frozen::map<operation, std::string_view, (size_t)operation::_count> op_resolve_table = {
    {operation::add, "add"},
    {operation::sub, "subtract"},
    {operation::mul, "multiply"},
    {operation::min, "min"},
    {operation::max, "max"},
};

constexpr operation op_from_string(std::string_view topic)
{
    auto it = op_lookup_table.find(topic);
    if (it == op_lookup_table.end()) {
        return operation::add;
    }

    return it->second;
}

constexpr std::string_view op_to_string(operation topic)
{
    auto it = op_resolve_table.find(topic);
    if (it == op_resolve_table.end()) {
        return "add";
    }

    return it->second;
}

auto operation_setter = [](operation& t, const nlohmann::json& j) -> bool {
    try {
        t = op_from_string(j.get<std::string_view>());
        return true;
    } catch (nlohmann::json::exception& e) {
        return false;
    }
};

auto operation_getter = [](const operation& t) -> nlohmann::json { return op_to_string(t); };

} // namespace miximus::nodes::math