#include "nodes/dummy/dummy.hpp"
#include "nodes/interface.hpp"
#include "nodes/option_typed.hpp"
#include "utils/bind.hpp"

namespace miximus::nodes::dummy {

class node_impl : public node_i
{
    using dir = interface_i::dir;

    node_type_e          type_;
    option_typed<double> opt_test_{};
    interface<double>    iface_input_{dir::input};
    interface<double>    iface_output_{dir::output};

  public:
    explicit node_impl(node_type_e type)
        : type_(type)
    {
        options_.emplace("test", &opt_test_);
        interfaces_.emplace("ip", &iface_input_);
        interfaces_.emplace("op", &iface_output_);
    }

    void prepare() final {}

    void execute(node_map_t& nodes, con_map_t& connections) final
    {
        iface_input_.resolve_connection_value(nodes, connections["ip"]);
        iface_output_.set_value(opt_test_.get_value());
    }

    void complete() final { node_i::complete(); }

    node_type_e type() final { return type_; }
};

std::shared_ptr<node_i> create_node(node_type_e type) { return std::make_shared<node_impl>(type); }

} // namespace miximus::nodes::dummy
