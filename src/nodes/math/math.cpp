#include "nodes/math/math.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/validate_option.hpp"

#include <glm/common.hpp>

#include <functional>

namespace {
using namespace miximus;
using namespace miximus::nodes;

template <typename T>
class node_impl : public node_i
{
    input_interface_s<T>   iface_a_;
    input_interface_s<T>   iface_b_;
    output_interface_s<T>  iface_res_;
    const std::string_view type_;
    const std::string_view name_;

  public:
    explicit node_impl(std::string_view type, std::string_view name)
        : type_(type)
        , name_(name)
    {
        interfaces_.emplace("a", &iface_a_);
        interfaces_.emplace("b", &iface_b_);
        interfaces_.emplace("res", &iface_res_);
    }

    void prepare(core::app_state_s* /*app*/, const node_state_s& /*nodes*/, traits_s* /*traits*/) final {}

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        T res{};

        auto op    = state.get_option<std::string_view>("operation", "add");
        auto a_opt = state.get_option<T>("a");
        auto b_opt = state.get_option<T>("b");

        auto a = iface_a_.resolve_value(app, nodes, state.get_connection_set("a"), a_opt);
        auto b = iface_b_.resolve_value(app, nodes, state.get_connection_set("b"), b_opt);

        if (op == "add") {
            res = a + b;
        } else if (op == "sub") {
            res = a - b;
        } else if (op == "mul") {
            res = a * b;
        } else if (op == "min") {
            res = glm::min(a, b);
        } else if (op == "max") {
            res = glm::max(a, b);
        }

        iface_res_.set_value(res);
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", name_},
            {"operation", "add"},
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "operation") {
            if (!validate_option<std::string_view>(value)) {
                return false;
            }

            auto val = value->get<std::string_view>();
            if (val == "add" || val == "sub" || val == "mul" || val == "min" || val == "max") {
                return true;
            }
        } else if (name == "a" || name == "b") {
            return validate_option<T>(value);
        }

        return false;
    }

    std::string_view type() const final { return type_; }
};

} // namespace

namespace miximus::nodes::math {

std::shared_ptr<node_i> create_math_f64_node()
{
    return std::make_shared<node_impl<double>>("math_f64", "Number math");
}

std::shared_ptr<node_i> create_math_vec2_node()
{
    return std::make_shared<node_impl<gpu::vec2_t>>("math_vec2", "Vector math");
}

} // namespace miximus::nodes::math
