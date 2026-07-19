#include "types/settings_option.hpp"

#include <nlohmann/json.hpp>

namespace miximus {

void to_json(nlohmann::json& json, const settings_option_s& option)
{
    json = {
        {"id",    option.id   },
        {"label", option.label},
    };
}

void from_json(const nlohmann::json& json, settings_option_s& option)
{
    json.at("id").get_to(option.id);
    json.at("label").get_to(option.label);
}

std::vector<settings_option_s> make_settings_options_with_matching_labels(const std::vector<std::string>& ids)
{
    std::vector<settings_option_s> options;
    options.reserve(ids.size());
    for (const auto& id : ids) {
        options.push_back({.id = id, .label = id});
    }
    return options;
}

} // namespace miximus
