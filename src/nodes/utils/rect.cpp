#include "gpu/types.hpp"
#include "nodes/interface.hpp"
#include "nodes/validate_option.hpp"
#include "utils.hpp"

namespace {
using namespace miximus;
using namespace miximus::nodes;

class node_impl : public node_i
{
    input_interface_s<gpu::vec2_t>  iface_pos_;
    input_interface_s<gpu::vec2_t>  iface_size_;
    output_interface_s<gpu::rect_s> iface_res_;

  public:
    explicit node_impl()
    {
        interfaces_.emplace("pos", &iface_pos_);
        interfaces_.emplace("size", &iface_size_);
        interfaces_.emplace("res", &iface_res_);
    }

    void prepare(core::app_state_s* /*app*/, const node_state_s& /*nodes*/, traits_s* /*traits*/) final {}

    void execute(core::app_state_s* app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto pos_opt  = state.get_option<gpu::vec2_t>("pos", {0, 0});
        auto size_opt = state.get_option<gpu::vec2_t>("size", {1, 1});

        auto pos  = iface_pos_.resolve_value(app, nodes, state.get_connection_set("pos"), pos_opt);
        auto size = iface_size_.resolve_value(app, nodes, state.get_connection_set("size"), size_opt);

        iface_res_.set_value({pos, size});
    }

    nlohmann::json get_default_options() const final
    {
        return {
            {"name", "Rectangle"},
            {"pos", nlohmann::json::array({0, 0})},
            {"size", nlohmann::json::array({1, 1})},
        };
    }

    bool test_option(std::string_view name, nlohmann::json* value) const final
    {
        if (name == "pos" || name == "size") {
            return validate_option<gpu::vec2_t>(value);
        }

        return false;
    }

    std::string_view type() const final { return "rect"; }
};

} // namespace

namespace miximus::nodes::utils {

std::shared_ptr<node_i> create_rect_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::utils
