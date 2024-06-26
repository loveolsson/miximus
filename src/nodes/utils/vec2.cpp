#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/validate_option.hpp"

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    input_interface_s<double>       iface_x_{"x"};
    input_interface_s<double>       iface_y_{"y"};
    output_interface_s<gpu::vec2_t> iface_res_{"res"};

  public:
    explicit node_impl()
    {
        register_interface(&iface_x_);
        register_interface(&iface_y_);
        register_interface(&iface_res_);
    }

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto x = state.get_option<double>("x", 0);
        auto y = state.get_option<double>("y", 0);

        x = iface_x_.resolve_value(app, nodes, state, x);
        y = iface_y_.resolve_value(app, nodes, state, y);

        iface_res_.set_value({x, y});
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Vec2"},
            {"x",    0     },
            {"y",    0     },
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "x" || name == "y") {
            return validate_option<double>(value);
        }

        return false;
    }

    std::string_view type() const final { return "vec2"; }
};

} // namespace

namespace miximus::nodes::utils {

std::shared_ptr<node_i> create_vec2_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::utils
