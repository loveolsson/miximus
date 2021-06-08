#include "nodes/dummy/dummy.hpp"
#include "nodes/interface.hpp"
#include "nodes/option_typed.hpp"

namespace miximus::nodes::dummy {

class node_impl : public node
{
    option_typed<double>           opt_test;
    interface_typed<double, true>  iface_input{true};
    interface_typed<double, false> iface_output{false};

  public:
    node_impl()
        : node()
    {
        options_.emplace("test", &opt_test);
        interfaces_.emplace("ip", &iface_input);
        interfaces_.emplace("op", &iface_output);
    }

    void prepare() final {}

    void execute(const node_cfg_t& cfg) final
    {
        auto input_vals = iface_input.resolve_connection_values(cfg);
        iface_output.set_value(opt_test.get_value());
    }

    void complete() final { node::complete(); }

    node_type_e type() final { return node_type_e::invalid; }
};

std::shared_ptr<node> create_node() { return std::make_shared<node_impl>(); }

} // namespace miximus::nodes::dummy
