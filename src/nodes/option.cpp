#include "nodes/option.hpp"
#include "nodes/option_typed.hpp"

namespace miximus::nodes {

bool option_name::set_json(const nlohmann::json& value) { return default_option_setter(name_, value); }

nlohmann::json option_name::get_json() const { return name_; }

bool option_position::set_json(const nlohmann::json& value)
{
    if (value.is_array() && value.size() == 2) {
        auto& x = value[0];
        auto& y = value[1];

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