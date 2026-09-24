#pragma once

#include "types/error.hpp"

#include <nlohmann/json.hpp>

#include <string>

namespace miximus::nodes {

// A handler's result, not completion of any background work it schedules.
struct action_result_s
{
    error_e        error{error_e::no_error};
    std::string    message{};
    nlohmann::json data = nullptr;
};

} // namespace miximus::nodes
