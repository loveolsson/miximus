#pragma once
#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

namespace miximus {

struct settings_option_s
{
    std::string id;
    std::string label;

    bool operator==(const settings_option_s&) const = default;
};

void to_json(nlohmann::json& json, const settings_option_s& option);
void from_json(const nlohmann::json& json, settings_option_s& option);

std::vector<settings_option_s> make_settings_options_with_matching_labels(const std::vector<std::string>& ids);

} // namespace miximus
