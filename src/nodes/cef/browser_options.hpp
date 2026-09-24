#pragma once

#include "gpu/types.hpp"
#include "nodes/normalize_option.hpp"

#include <cmath>

namespace miximus::nodes::cef {

inline nlohmann::json browser_default_options()
{
    return {
        {"name",    "Browser"              },
        {"enabled", true                   },
        {"url",     "about:blank"          },
        {"size",    gpu::vec2_t{1920, 1080}},
    };
}

inline option_result_e normalize_browser_option(std::string_view name, nlohmann::json* value)
{
    if (name == "enabled") {
        return normalize_option_value<bool>(value);
    }
    if (name == "size") {
        auto result = normalize_option_value<gpu::vec2_t>(value, gpu::vec2_t{1, 1}, gpu::vec2_t{4096, 4096});
        if (result == option_result_e::invalid) {
            return result;
        }
        for (auto& component : *value) {
            const auto rounded = std::round(component.get<double>());
            if (component != rounded) {
                component = rounded;
                result    = option_result_e::corrected;
            }
        }
        return result;
    }
    if (name == "url") {
        const auto result = normalize_option_value<std::string_view>(value);
        if (result == option_result_e::invalid || value->get_ref<const std::string&>().size() > 65536) {
            return option_result_e::invalid;
        }
        return result;
    }
    return option_result_e::invalid;
}
} // namespace miximus::nodes::cef
