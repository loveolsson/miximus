#include "nodes/dummy/dummy.hpp"
#include "nodes/interface.hpp"
#include "nodes/option_typed.hpp"
#include "utils/bind.hpp"

namespace miximus::nodes::dummy {

class node_impl : public node
{
    option_typed<double>    opt_test_{};
    interface_typed<double> iface_input_{true, true};
    interface_typed<double> iface_output_{false, false};

  public:
    node_impl()
        : node()
    {
        options_.emplace("test", &opt_test_);
        interfaces_.emplace("ip", &iface_input_);
        interfaces_.emplace("op", &iface_output_);
    }

    void prepare() final {}

    void execute(const node_cfg& cfg) final
    {
        auto input_vals = iface_input_.resolve_connection_values(cfg);
        iface_output_.set_value(opt_test_.get_value());
    }

    void complete() final { node::complete(); }

    node_type_e type() final { return node_type_e::invalid; }
};

std::shared_ptr<node> create_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::dummy
