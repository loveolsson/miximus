#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"

#include <glm/common.hpp>

#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtx/easing.hpp>
#undef GLM_ENABLE_EXPERIMENTAL

#include <cmath>
#include <cstdint>
#include <memory>

namespace {
using namespace miximus;
using namespace miximus::nodes;

enum class easing_e : uint8_t
{
    linear,
    quadratic_in,
    quadratic_out,
    quadratic_in_out,
    cubic_in,
    cubic_out,
    cubic_in_out,
    quartic_in,
    quartic_out,
    quartic_in_out,
    quintic_in,
    quintic_out,
    quintic_in_out,
    sine_in,
    sine_out,
    sine_in_out,
    circular_in,
    circular_out,
    circular_in_out,
    exponential_in,
    exponential_out,
    exponential_in_out,
    elastic_in,
    elastic_out,
    elastic_in_out,
    back_in,
    back_out,
    back_in_out,
    bounce_in,
    bounce_out,
    bounce_in_out,
};

double ease(double value, easing_e easing)
{
    switch (easing) {
        case easing_e::linear:
            return glm::linearInterpolation(value);
        case easing_e::quadratic_in:
            return glm::quadraticEaseIn(value);
        case easing_e::quadratic_out:
            return glm::quadraticEaseOut(value);
        case easing_e::quadratic_in_out:
            return glm::quadraticEaseInOut(value);
        case easing_e::cubic_in:
            return glm::cubicEaseIn(value);
        case easing_e::cubic_out:
            return glm::cubicEaseOut(value);
        case easing_e::cubic_in_out:
            return glm::cubicEaseInOut(value);
        case easing_e::quartic_in:
            return glm::quarticEaseIn(value);
        case easing_e::quartic_out:
            return glm::quarticEaseOut(value);
        case easing_e::quartic_in_out:
            return glm::quarticEaseInOut(value);
        case easing_e::quintic_in:
            return glm::quinticEaseIn(value);
        case easing_e::quintic_out:
            return glm::quinticEaseOut(value);
        case easing_e::quintic_in_out:
            return glm::quinticEaseInOut(value);
        case easing_e::sine_in:
            return glm::sineEaseIn(value);
        case easing_e::sine_out:
            return glm::sineEaseOut(value);
        case easing_e::sine_in_out:
            return glm::sineEaseInOut(value);
        case easing_e::circular_in:
            return glm::circularEaseIn(value);
        case easing_e::circular_out:
            return glm::circularEaseOut(value);
        case easing_e::circular_in_out:
            return glm::circularEaseInOut(value);
        case easing_e::exponential_in:
            return glm::exponentialEaseIn(value);
        case easing_e::exponential_out:
            return glm::exponentialEaseOut(value);
        case easing_e::exponential_in_out:
            return glm::exponentialEaseInOut(value);
        case easing_e::elastic_in:
            return glm::elasticEaseIn(value);
        case easing_e::elastic_out:
            return glm::elasticEaseOut(value);
        case easing_e::elastic_in_out:
            return glm::elasticEaseInOut(value);
        case easing_e::back_in:
            return glm::backEaseIn(value);
        case easing_e::back_out:
            return glm::backEaseOut(value);
        case easing_e::back_in_out:
            return glm::backEaseInOut(value);
        case easing_e::bounce_in:
            return glm::bounceEaseIn(value);
        case easing_e::bounce_out:
            return glm::bounceEaseOut(value);
        case easing_e::bounce_in_out:
            return glm::bounceEaseInOut(value);
    }

    return value;
}

class node_impl : public node_i
{
    input_interface_s<double>  iface_t_{*this, "t"};
    output_interface_s<double> iface_res_{*this, "res"};

  public:
    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        const auto option = state.get_option<double>("t");
        const auto input  = iface_t_.resolve_value(app, nodes, state, option);
        const auto value  = glm::clamp(input, 0.0, 1.0);
        const auto easing = state.get_enum_option("easing", easing_e::linear);

        iface_res_.set_value(ease(value, easing));
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",   "Easing"                        },
            {"t",      0                               },
            {"easing", enum_to_string(easing_e::linear)},
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "t") {
            return normalize_option_value<double>(value, 0, 1);
        }
        if (name == "easing") {
            const auto result = normalize_option_value<std::string_view>(value);
            if (result == option_result_e::invalid) {
                return result;
            }

            return enum_from_string<easing_e>(value->get<std::string_view>()).has_value() ? result
                                                                                          : option_result_e::invalid;
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return "easing_f64"; }
};

} // namespace

namespace miximus::nodes::math {

std::shared_ptr<node_i> create_easing_f64_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::math
