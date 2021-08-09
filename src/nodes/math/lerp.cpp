#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/math/math.hpp"
#include "nodes/validate_option.hpp"

#include <glm/glm.hpp>

#include <functional>

namespace {
using namespace miximus;
using namespace miximus::nodes;

template <typename T>
T lerp(const T& a, const T& b, double t)
{
    return glm::mix(a, b, t);
}

template <>
gpu::rect_s lerp<gpu::rect_s>(const gpu::rect_s& a, const gpu::rect_s& b, double t)
{
    gpu::rect_s res;
    res.pos  = lerp(a.pos, b.pos, t);
    res.size = lerp(a.size, b.size, t);
    return res;
}

template <typename T>
class node_impl : public node_i
{
    input_interface_s<T>      iface_a_;
    input_interface_s<T>      iface_b_;
    input_interface_s<double> iface_t_;
    output_interface_s<T>     iface_res_;
    const std::string_view    type_;
    const std::string_view    name_;

  public:
    explicit node_impl(std::string_view type, std::string_view name)
        : type_(type)
        , name_(name)
    {
        interfaces_.emplace("a", &iface_a_);
        interfaces_.emplace("b", &iface_b_);
        interfaces_.emplace("t", &iface_t_);
        interfaces_.emplace("res", &iface_res_);
    }

    void prepare(core::app_state_s* /*app*/, const node_state_s& /*nodes*/, traits_s* /*traits*/) final {}

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto a_opt = state.get_option<T>("a");
        auto b_opt = state.get_option<T>("b");
        auto t_opt = state.get_option<double>("t");

        auto a = iface_a_.resolve_value(app, nodes, state.get_connection_set("a"), a_opt);
        auto b = iface_b_.resolve_value(app, nodes, state.get_connection_set("b"), b_opt);
        auto t = iface_t_.resolve_value(app, nodes, state.get_connection_set("t"), t_opt);

        t = glm::clamp(t, 0.0, 1.0);

        T res = lerp(a, b, t);

        iface_res_.set_value(res);
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", name_},
            {"operation", "add"},
        };
    }

    bool test_option(std::string_view name, const nlohmann::json& value) const final
    {
        if (name == "a" || name == "b") {
            return detail::validate_option<T>(value);
        }

        if (name == "t") {
            return detail::validate_option<double>(value);
        }

        return false;
    }

    std::string_view type() const final { return type_; }
};

} // namespace

namespace miximus::nodes::math {

std::shared_ptr<node_i> create_lerp_f64_node()
{
    return std::make_shared<node_impl<double>>("lerp_f64", "Number lerp");
}

std::shared_ptr<node_i> create_lerp_vec2_node()
{
    return std::make_shared<node_impl<gpu::vec2_t>>("lerp_vec2", "Vector lerp");
}

std::shared_ptr<node_i> create_lerp_rect_node()
{
    return std::make_shared<node_impl<gpu::rect_s>>("lerp_rect", "Rectangle lerp");
}

} // namespace miximus::nodes::math
