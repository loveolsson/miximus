#include "nodes/option.hpp"
#include "nodes/option_typed.hpp"

namespace miximus::nodes {

option_name::~option_name()
{
    if (!name_.empty()) {
        names_in_use.erase(name_);
    }
}

bool option_name::set_json(const nlohmann::json& value)
{
    if (!value.is_string()) {
        return false;
    }

    std::string new_name = value;

    if (name_ == new_name) {
        return true;
    }

    if (names_in_use.count(new_name) > 0) {
        return false;
    }

    names_in_use.emplace(new_name);
    name_ = new_name;
    return true;
}

nlohmann::json option_name::get_json() const { return name_; }

bool option_position::set_json(const nlohmann::json& value)
{
    if (value.is_array() && value.size() == 2) {
        const auto& x = value[0];
        const auto& y = value[1];

        if (x.is_number() && y.is_number()) {
            x_ = x;
            y_ = y;

            return true;
        }
    }

    return false;
}

nlohmann::json option_position::get_json() const { return nlohmann::json::array({x_, y_}); }

} // namespace miximus::nodes