#include "nodes/math/math.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/math/operation.hpp"
#include "nodes/option_typed.hpp"

#include <glm/glm.hpp>

#include <functional>

namespace miximus::nodes::math {

template <typename T>
class node_impl : public node_i
{
    using dir = interface_i::dir;

    const node_type_e type_;

    option_typed<operation_e> operation_{operation_setter, operation_getter};

    interface<T> iface_a_{dir::input};
    interface<T> iface_b_{dir::input};
    interface<T> iface_res_{dir::output};

  public:
    explicit node_impl(node_type_e type)
        : type_(type)
    {
        options_.emplace("operation", &operation_);
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

        switch (operation_.get_value()) {
            case operation_e::add:
                res = a + b;
                break;
            case operation_e::sub:
                res = a - b;
                break;
            case operation_e::mul:
                res = a * b;
                break;
            case operation_e::min:
                res = glm::min(a, b);
                break;
            case operation_e::max:
                res = glm::max(a, b);
                break;
            default:
                break;
        }

        iface_res_.set_value(res);
    }

    void complete() final { node_i::complete(); }

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
