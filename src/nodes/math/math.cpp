#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "nodes/normalize_option.hpp"

#include <cstdint>
#include <glm/common.hpp>
#include <memory>
#include <string_view>

namespace {
using namespace miximus;
using namespace miximus::nodes;

enum class operation_e : uint8_t
{
    add,
    sub,
    mul,
    min,
    max,
};

template <typename T>
class node_impl : public node_i
{
    input_interface_s<T>  iface_a_{*this, "a"};
    input_interface_s<T>  iface_b_{*this, "b"};
    output_interface_s<T> iface_res_{*this, "res"};
    std::string_view      type_;
    std::string_view      name_;

  public:
    explicit node_impl(std::string_view type, std::string_view name)
        : type_(type)
        , name_(name)
    {
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        T res{};

        auto op = state.get_enum_option("operation", operation_e::add);

        auto a_opt = state.get_option<T>("a");
        auto b_opt = state.get_option<T>("b");

        auto a = iface_a_.resolve_value(app, nodes, state, a_opt);
        auto b = iface_b_.resolve_value(app, nodes, state, b_opt);

        switch (op) {
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

    nlohmann::json get_default_options() const final
    {
        return {
            {"name",      name_                           },
            {"operation", enum_to_string(operation_e::add)},
        };
    }

    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "operation") {
            const auto result = normalize_option_value<std::string_view>(value);
            if (result == option_result_e::invalid) {
                return result;
            }

            const auto val = value->get<std::string_view>();
            return enum_from_string<operation_e>(val).has_value() ? result : option_result_e::invalid;
        }

        if (name == "a" || name == "b") {
            return normalize_option_value<T>(value);
        }

        return option_result_e::invalid;
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
