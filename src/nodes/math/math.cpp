#include "nodes/math/math.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"

#include <glm/glm.hpp>

#include <functional>

namespace {

template <typename T>
bool validate_num_option(const nlohmann::json&);

template <>
bool validate_num_option<int64_t>(const nlohmann::json& val)
{
    return val.is_number_integer();
}

template <>
bool validate_num_option<double>(const nlohmann::json& val)
{
    return val.is_number();
}

template <>
bool validate_num_option<miximus::gpu::vec2>(const nlohmann::json& val)
{
    if (!val.is_array() || val.size() != 2) {
        return false;
    }
    if (!val[0].is_number() || !val[1].is_number()) {
        return false;
    }
    return true;
}

} // namespace

namespace miximus::nodes::math {

template <typename T, node_type_e Type>
class node_impl : public node_i
{
    using dir = interface_i::dir_e;

    input_interface_s<T>  iface_a_;
    input_interface_s<T>  iface_b_;
    output_interface_s<T> iface_res_;

  public:
    explicit node_impl()
    {
        interfaces_.emplace("a", &iface_a_);
        interfaces_.emplace("b", &iface_b_);
        interfaces_.emplace("res", &iface_res_);
    }

    bool prepare(app_state_s&, const node_state_s&) final { return true; }

    void execute(app_state_s& app, const node_map_t& nodes, const node_state_s& state) final
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

    void complete() final { node_i::complete(); }

    nlohmann::json get_default_options() const final
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

    bool check_option(std::string_view name, const nlohmann::json& value) const final
    {
        if (name == "operation") {
            if (!value.is_string()) {
                return false;
            }

            auto val = value.get<std::string_view>();
            if (val == "add" || val == "sub" || val == "mul" || val == "min" || val == "max") {
                return true;
            }
        } else if (name == "a" || name == "b") {
            return validate_num_option<T>(value);
        }

        return false;
    }

    node_type_e type() const final { return Type; }
};

std::shared_ptr<node_i> create_node(node_type_e type)
{
    switch (type) {
        case node_type_e::math_f64:
            return std::make_shared<node_impl<double, node_type_e::math_f64>>();
        case node_type_e::math_i64:
            return std::make_shared<node_impl<int64_t, node_type_e::math_i64>>();
        case node_type_e::math_vec2:
            return std::make_shared<node_impl<gpu::vec2, node_type_e::math_vec2>>();

        default:
            return nullptr;
    };
}

} // namespace miximus::nodes::math
