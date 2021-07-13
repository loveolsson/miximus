#include "nodes/math/math.hpp"
#include "gpu/types.hpp"
#include "nodes/interface.hpp"

#include <glm/glm.hpp>

#include <functional>

namespace miximus::nodes::math {

template <typename T, auto Op>
class node_impl : public node
{
    using dir = interface::dir;

    interface_typed<T> iface_a_{dir::input};
    interface_typed<T> iface_b_{dir::input};
    interface_typed<T> iface_res_{dir::output};

    node_type_e type_;

  public:
    explicit node_impl(node_type_e type)
        : node()
        , type_(type)
    {
        interfaces_.emplace("a", &iface_a_);
        interfaces_.emplace("b", &iface_b_);
        interfaces_.emplace("res", &iface_res_);
    }

    void prepare() final {}

    void execute(const node_cfg& cfg) final
    {
        iface_a_.resolve_connection_value(cfg);
        iface_b_.resolve_connection_value(cfg);

        T res = Op(iface_a_.get_value(), iface_b_.get_value());
        iface_res_.set_value(res);
    }

    void complete() final { node::complete(); }

    node_type_e type() final { return type_; }
};

template <typename T>
T add(const T& a, const T& b)
{
    return a + b;
}

template <typename T>
T sub(const T& a, const T& b)
{
    return a - b;
}

template <typename T>
T mul(const T& a, const T& b)
{
    return a * b;
}

template <typename T>
T min(const T& a, const T& b)
{
    return glm::min(a, b);
}

template <typename T>
T max(const T& a, const T& b)
{
    return glm::max(a, b);
}

std::shared_ptr<node> create_node(node_type_e type)
{
    switch (type) {
        // ADD
        case node_type_e::math_add_f64:
            return std::make_shared<node_impl<double, add<double>>>(node_type_e::math_add_f64);
        case node_type_e::math_add_i64:
            return std::make_shared<node_impl<int64_t, add<int64_t>>>(node_type_e::math_add_i64);
        case node_type_e::math_add_vec2:
            return std::make_shared<node_impl<gpu::vec2, add<gpu::vec2>>>(node_type_e::math_add_vec2);

        // SUB
        case node_type_e::math_sub_f64:
            return std::make_shared<node_impl<double, sub<double>>>(node_type_e::math_sub_f64);
        case node_type_e::math_sub_i64:
            return std::make_shared<node_impl<int64_t, sub<int64_t>>>(node_type_e::math_sub_i64);
        case node_type_e::math_sub_vec2:
            return std::make_shared<node_impl<gpu::vec2, sub<gpu::vec2>>>(node_type_e::math_sub_vec2);

        // MUL
        case node_type_e::math_mul_f64:
            return std::make_shared<node_impl<double, mul<double>>>(node_type_e::math_mul_f64);
        case node_type_e::math_mul_i64:
            return std::make_shared<node_impl<int64_t, mul<int64_t>>>(node_type_e::math_mul_i64);
        case node_type_e::math_mul_vec2:
            return std::make_shared<node_impl<gpu::vec2, mul<gpu::vec2>>>(node_type_e::math_mul_vec2);

        // MIN
        case node_type_e::math_min_f64:
            return std::make_shared<node_impl<double, min<double>>>(node_type_e::math_min_f64);
        case node_type_e::math_min_i64:
            return std::make_shared<node_impl<int64_t, min<int64_t>>>(node_type_e::math_min_i64);
        case node_type_e::math_min_vec2:
            return std::make_shared<node_impl<gpu::vec2, min<gpu::vec2>>>(node_type_e::math_min_vec2);

        // MAX
        case node_type_e::math_max_f64:
            return std::make_shared<node_impl<double, max<double>>>(node_type_e::math_max_f64);
        case node_type_e::math_max_i64:
            return std::make_shared<node_impl<int64_t, max<int64_t>>>(node_type_e::math_max_i64);
        case node_type_e::math_max_vec2:
            return std::make_shared<node_impl<gpu::vec2, max<gpu::vec2>>>(node_type_e::math_max_vec2);

        default:
            return nullptr;
    };
}

} // namespace miximus::nodes::math
