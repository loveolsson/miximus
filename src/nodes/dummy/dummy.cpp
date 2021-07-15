#include "nodes/dummy/dummy.hpp"
#include "nodes/interface.hpp"
#include "utils/bind.hpp"

namespace miximus::nodes::dummy {

class node_impl : public node_i
{
    using dir = interface_i::dir;

    node_type_e       type_;
    interface<double> iface_input_{dir::input};
    interface<double> iface_output_{dir::output};

  public:
    explicit node_impl(node_type_e type)
        : type_(type)
    {
        interfaces_.emplace("ip", &iface_input_);
        interfaces_.emplace("op", &iface_output_);
    }

    bool prepare() final { return false; }

    void execute(const node_map_t& nodes, const node_state& state) final
    {
        iface_input_.resolve_connection_value(nodes, state.get_connections("ip"));
        iface_output_.set_value(iface_input_.get_value());
    }

    void complete() final { node_i::complete(); }

    nlohmann::json get_default_options() final { return {}; }

    bool check_option(std::string_view /*name*/, const nlohmann::json& /*value*/) final { return false; }

    node_type_e type() final { return type_; }
};

std::shared_ptr<node_i> create_node(node_type_e type) { return std::make_shared<node_impl>(type); }

} // namespace miximus::nodes::dummy
