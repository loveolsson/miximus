#include "nodes/math/math.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"

#include <glm/glm.hpp>

#include <functional>

namespace miximus::nodes::math {

template <typename T>
class node_impl : public node_i
{
    using dir = interface_i::dir;

    const node_type_e type_;

    input_interface<T>  iface_a_;
    input_interface<T>  iface_b_;
    output_interface<T> iface_res_;

  public:
    explicit node_impl(node_type_e type)
        : type_(type)
    {
        interfaces_.emplace("a", &iface_a_);
        interfaces_.emplace("b", &iface_b_);
        interfaces_.emplace("res", &iface_res_);
    }

    bool prepare(const node_state&) final { return true; }

    void execute(node_map_t& nodes, const node_state& state) final
    {
        T res{};

        auto a = iface_a_.resolve_value(nodes, state.get_connections("a"));
        auto b = iface_b_.resolve_value(nodes, state.get_connections("b"));

        auto op = state.get_option<std::string_view>("operation", "add");

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

    void complete() final { node_i::complete(); }

    nlohmann::json get_default_options() final
    {
        std::string_view name("Math");
        switch (type()) {
            case node_type_e::math_f64:
                name = "Floating point math";
                break;
            case node_type_e::math_i64:
                name = "Integer math";
                break;
            case node_type_e::math_vec2:
                name = "Vector math";
                break;
            default:
                break;
        }

        return {
            {"name", name},
            {"operation", "add"},
        };
    }

    bool check_option(std::string_view name, const nlohmann::json& value) final
    {
        if (name == "operation") {
            if (!value.is_string()) {
                return false;
            }

            auto val = value.get<std::string_view>();
            if (val == "add" || val == "sub" || val == "mul" || val == "min" || val == "max") {
                return true;
            }
        }

        return false;
    }

    node_type_e type() final { return type_; }
};

std::shared_ptr<node_i> create_node(node_type_e type)
{
    switch (type) {
        case node_type_e::math_f64:
            return std::make_shared<node_impl<double>>(node_type_e::math_f64);
        case node_type_e::math_i64:
            return std::make_shared<node_impl<int64_t>>(node_type_e::math_i64);
        case node_type_e::math_vec2:
            return std::make_shared<node_impl<gpu::vec2>>(node_type_e::math_vec2);

        default:
            return nullptr;
    };
}

} // namespace miximus::nodes::math
