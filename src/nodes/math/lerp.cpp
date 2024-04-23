#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/validate_option.hpp"

#include <glm/glm.hpp>

namespace {
using namespace miximus;
using namespace miximus::nodes;

template <typename T>
inline T lerp(const T& a, const T& b, double t)
{
    return glm::mix(a, b, t);
}

template <>
inline gpu::rect_s lerp<gpu::rect_s>(const gpu::rect_s& a, const gpu::rect_s& b, double t)
{
    gpu::rect_s res;
    res.pos  = lerp(a.pos, b.pos, t);
    res.size = lerp(a.size, b.size, t);
    return res;
}

template <typename T>
class node_impl : public node_i
{
    input_interface_s<T>      iface_a_{"a"};
    input_interface_s<T>      iface_b_{"b"};
    input_interface_s<double> iface_t_{"t"};
    output_interface_s<T>     iface_res_{"res"};
    std::string_view          type_;
    std::string_view          name_;

  public:
    explicit node_impl(std::string_view type, std::string_view name)
        : type_(type)
        , name_(name)
    {
        register_interface(&iface_a_);
        register_interface(&iface_b_);
        register_interface(&iface_t_);
        register_interface(&iface_res_);
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto a_opt = state.get_option<T>("a");
        auto b_opt = state.get_option<T>("b");
        auto t_opt = state.get_option<double>("t");

        auto a = iface_a_.resolve_value(app, nodes, state, a_opt);
        auto b = iface_b_.resolve_value(app, nodes, state, b_opt);
        auto t = iface_t_.resolve_value(app, nodes, state, t_opt);

        t = glm::clamp(t, 0.0, 1.0);

        T res = lerp(a, b, t);

        iface_res_.set_value(res);
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", name_},
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "a" || name == "b") {
            return validate_option<T>(value);
        }

        if (name == "t") {
            return validate_option<double>(value);
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
