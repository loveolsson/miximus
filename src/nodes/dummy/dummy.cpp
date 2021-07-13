#include "nodes/dummy/dummy.hpp"
#include "nodes/interface.hpp"
#include "nodes/option_typed.hpp"
#include "utils/bind.hpp"

namespace miximus::nodes::dummy {

class node_impl : public node
{
    using dir = interface::dir;

    node_type_e             type_;
    option_typed<double>    opt_test_{};
    interface_typed<double> iface_input_{dir::input};
    interface_typed<double> iface_output_{dir::output};

  public:
    explicit node_impl(node_type_e type)
        : type_(type)
    {
        options_.emplace("test", &opt_test_);
        interfaces_.emplace("ip", &iface_input_);
        interfaces_.emplace("op", &iface_output_);
    }

    void prepare() final {}

    void execute(const node_cfg& cfg) final
    {
        iface_input_.resolve_connection_value(cfg);
        iface_output_.set_value(opt_test_.get_value());
    }

    void complete() final { node::complete(); }

    node_type_e type() final { return type_; }
};

std::shared_ptr<node> create_node(node_type_e type) { return std::make_shared<node_impl>(type); }

} // namespace miximus::nodes::dummy
