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

    interface<T> iface_a_{dir::input};
    interface<T> iface_b_{dir::input};
    interface<T> iface_res_{dir::output};

  public:
    explicit node_impl(node_type_e type)
        : type_(type)
    {
        interfaces_.emplace("a", &iface_a_);
        interfaces_.emplace("b", &iface_b_);
        interfaces_.emplace("res", &iface_res_);
    }

    bool prepare() final { return false; }

    void execute(node_map_t& nodes, node_state& state) final
    {
        iface_a_.resolve_connection_value(nodes, state.con_map["a"]);
        iface_b_.resolve_connection_value(nodes, state.con_map["b"]);

        T res{};
        T a = iface_a_.get_value();
        T b = iface_b_.get_value();

        auto op = state.options["operation"].get<std::string_view>();

        if (op == "sub") {
            res = a - b;
        } else if (op == "mul") {
            res = a * b;
        } else if (op == "min") {
            res = glm::min(a, b);
        } else if (op == "max") {
            res = glm::max(a, b);
        } else { // add
            res = a + b;
        }

        iface_res_.set_value(res);
    }

    void complete() final { node_i::complete(); }

    nlohmann::json get_default_options() final { return {{"operation", "add"}}; }

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
