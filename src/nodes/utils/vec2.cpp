#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/validate_option.hpp"
#include "utils.hpp"

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    input_interface_s<double>       iface_x_;
    input_interface_s<double>       iface_y_;
    output_interface_s<gpu::vec2_t> iface_res_;

  public:
    explicit node_impl()
    {
        interfaces_.emplace("x", &iface_x_);
        interfaces_.emplace("y", &iface_y_);
        interfaces_.emplace("res", &iface_res_);
    }

    void prepare(core::app_state_s* /*app*/, const node_state_s& /*nodes*/, traits_s* /*traits*/) final {}

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto x_opt = state.get_option<double>("x", 0);
        auto y_opt = state.get_option<double>("y", 0);

        auto x = iface_x_.resolve_value(app, nodes, state.get_connection_set("x"), x_opt);
        auto y = iface_y_.resolve_value(app, nodes, state.get_connection_set("y"), y_opt);

        iface_res_.set_value({x, y});
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Vec2"},
            {"x", 0},
            {"y", 0},
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
