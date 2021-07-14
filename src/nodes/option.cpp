#include "nodes/option.hpp"
#include "logger/logger.hpp"
#include "nodes/option_typed.hpp"

namespace miximus::nodes {

option_name::~option_name()
{
    std::unique_lock lock(names_in_use_mutex_);

    if (!name_.empty()) {
        names_in_use.erase(name_);
    }
}

bool option_name::set_json(const nlohmann::json& value)
{
    std::unique_lock lock(value_mutex_);

    if (!value.is_string()) {
        spdlog::get("app")->warn("Option value is not a string");
        return false;
    }

    std::string new_name = value;

    if (name_ == new_name) {
        return true;
    }

    {
        std::unique_lock lock(names_in_use_mutex_);

        if (names_in_use.count(new_name) > 0) {
            spdlog::get("app")->warn(R"(Node name "{}" already in use)", new_name);
            return false;
        }

        names_in_use.erase(name_);
        names_in_use.emplace(new_name);
    }

    name_ = new_name;
    return true;
}

nlohmann::json option_name::get_json() const { return get_value(); }

std::string option_name::get_value() const
{
    std::unique_lock lock(value_mutex_);
    return name_;
}

bool option_position::set_json(const nlohmann::json& value)
{
    std::unique_lock lock(value_mutex_);

    if (!value.is_array() || value.size() != 2) {
        spdlog::get("app")->warn("Option value is not an array with 2 items");
        return false;
    }

    const auto& x = value[0];
    const auto& y = value[1];

    if (!x.is_number() || !y.is_number()) {
        spdlog::get("app")->warn("Option values not numbers");
        return false;
    }

    pos_ = {x, y};

    return true;
}

nlohmann::json option_position::get_json() const
{
    auto pos = get_value();
    return nlohmann::json::array({pos[0], pos[1]});
}

gpu::vec2 option_position::get_value() const
{
    std::unique_lock lock(value_mutex_);
    return pos_;
}

} // namespace miximus::nodes