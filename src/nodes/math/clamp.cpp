#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"

#include <glm/common.hpp>
#include <memory>
#include <string_view>
#include <utility>

namespace {
using namespace miximus;
using namespace miximus::nodes;

constexpr gpu::rect_s DEFAULT_RECT_VALUE{};
constexpr gpu::rect_s DEFAULT_RECT_MIN{
    .pos  = {0, 0},
    .size = {0, 0}
};
constexpr gpu::rect_s DEFAULT_RECT_MAX{
    .pos  = {1, 1},
    .size = {1, 1}
};

template <typename T>
T clamp_value(const T& value, const T& min, const T& max)
{
    return glm::clamp(value, min, max);
}

template <>
gpu::rect_s clamp_value(const gpu::rect_s& value, const gpu::rect_s& min, const gpu::rect_s& max)
{
    return {
        .pos  = glm::clamp(value.pos, min.pos, max.pos),
        .size = glm::clamp(value.size, min.size, max.size),
    };
}

template <typename T>
class node_impl : public node_i
{
    input_interface_s<T>  iface_value_{*this, "value"};
    input_interface_s<T>  iface_min_{*this, "min"};
    input_interface_s<T>  iface_max_{*this, "max"};
    output_interface_s<T> iface_res_{*this, "res"};

    std::string_view type_;
    std::string_view name_;
    T                default_value_;
    T                default_min_;
    T                default_max_;

  public:
    node_impl(std::string_view type, std::string_view name, T default_value, T default_min, T default_max)
        : type_(type)
        , name_(name)
        , default_value_(std::move(default_value))
        , default_min_(std::move(default_min))
        , default_max_(std::move(default_max))
    {
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        const auto value_option = state.get_option<T>("value", default_value_);
        const auto min_option   = state.get_option<T>("min", default_min_);
        const auto max_option   = state.get_option<T>("max", default_max_);

        const auto value = iface_value_.resolve_value(app, nodes, state, value_option);
        const auto min   = iface_min_.resolve_value(app, nodes, state, min_option);
        const auto max   = iface_max_.resolve_value(app, nodes, state, max_option);

        iface_res_.set_value(clamp_value(value, min, max));
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",  name_         },
            {"value", default_value_},
            {"min",   default_min_  },
            {"max",   default_max_  },
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "value" || name == "min" || name == "max") {
            return normalize_option_value<T>(value);
        }

        return option_result_e::invalid;
    }

    std::string_view type() const final { return type_; }
};

} // namespace

namespace miximus::nodes::math {

std::shared_ptr<node_i> create_clamp_f64_node()
{
    return std::make_shared<node_impl<double>>("clamp_f64", "Number clamp", 0.0, 0.0, 1.0);
}

std::shared_ptr<node_i> create_clamp_vec2_node()
{
    return std::make_shared<node_impl<gpu::vec2_t>>(
        "clamp_vec2", "Vector clamp", gpu::vec2_t{0, 0}, gpu::vec2_t{0, 0}, gpu::vec2_t{1, 1});
}

std::shared_ptr<node_i> create_clamp_rect_node()
{
    return std::make_shared<node_impl<gpu::rect_s>>(
        "clamp_rect", "Rectangle clamp", DEFAULT_RECT_VALUE, DEFAULT_RECT_MIN, DEFAULT_RECT_MAX);
}

} // namespace miximus::nodes::math
