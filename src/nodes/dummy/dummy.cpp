#include "nodes/dummy/dummy.hpp"
#include "nodes/interface.hpp"
#include "utils/bind.hpp"

namespace miximus::nodes::dummy {

class node_impl : public node_i
{
    using dir = interface_i::dir_e;

    type_e                     type_;
    input_interface_s<double>  iface_input_;
    output_interface_s<double> iface_output_;

  public:
    explicit node_impl(type_e type)
        : type_(type)
    {
        interfaces_.emplace("ip", &iface_input_);
        interfaces_.emplace("op", &iface_output_);
    }

    bool prepare(core::app_state_s&, const node_state_s&) final { return false; }

    void execute(core::app_state_s& app, const node_map_t& nodes, const node_state_s& state) final
    {
        auto value = iface_input_.resolve_value(app, nodes, state.get_connection_set("ip"));
        iface_output_.set_value(value);
    }

    void complete() final { node_i::complete(); }

    bool test_option(std::string_view /*name*/, const nlohmann::json& /*value*/) const final { return false; }

    type_e type() const final { return type_; }
};

std::shared_ptr<node_i> create_node(node_i::type_e type) { return std::make_shared<node_impl>(type); }

} // namespace miximus::nodes::dummy
