#include "core/app_state.hpp"
#include "debug.hpp"
#include "nodes/interface.hpp"
#include "nodes/validate_option.hpp"

#include <cmath>

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    output_interface_s<double> iface_res_;

  public:
    explicit node_impl() { interfaces_.emplace("res", &iface_res_); }

    void prepare(core::app_state_s* /*app*/, const node_state_s& /*nodes*/, traits_s* /*traits*/) final {}

    void execute(core::app_state_s* app, const node_map_t& /*nodes*/, const node_state_s& state) final
    {
        auto size   = state.get_option<double>("size");
        auto center = state.get_option<double>("center");
        auto speed  = state.get_option<double>("speed");

        double s   = std::chrono::duration<double>(app->frame_info.pts.time_since_epoch()).count();
        double res = std::sin(s * speed) * size + center;

        iface_res_.set_value(res);
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Sinus source"},
            {"size", 1},
            {"center", 0},
            {"speed", 0.1},
        };
    }

    bool test_option(std::string_view name, const nlohmann::json& value) const final
    {
        if (name == "size" || name == "center" || name == "speed") {
            return detail::validate_option<double>(value);
        }

        return false;
    }

    std::string_view type() const final { return "sinus_source"; }
};

} // namespace

namespace miximus::nodes::debug {

std::shared_ptr<node_i> create_sinus_source_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::debug
