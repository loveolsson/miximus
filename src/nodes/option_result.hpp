#pragma once
#include "types/error.hpp"

namespace miximus::nodes {

enum class option_result_e
{
    ok,
    corrected,
    invalid,
};

constexpr option_result_e combine_option_results(option_result_e lhs, option_result_e rhs)
{
    if (lhs == option_result_e::invalid || rhs == option_result_e::invalid) {
        return option_result_e::invalid;
    }
    if (lhs == option_result_e::corrected || rhs == option_result_e::corrected) {
        return option_result_e::corrected;
    }
    return option_result_e::ok;
}

struct set_options_result_s
{
    error_e error;
    bool    has_corrected_values;
};

} // namespace miximus::nodes
