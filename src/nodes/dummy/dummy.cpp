#include "nodes/dummy/dummy.hpp"

#include <nlohmann/json.hpp>

namespace miximus::nodes::dummy {

class node_impl : public node
{
  public:
    node_impl(const std::string& id)
        : node(id)
    {
    }

    bool set_option(std::string_view option, const nlohmann::json& val) final
    {
        if (node::set_option(option, val)) {
            return true;
        }

        return false;
    }

    nlohmann::json get_options() final
    {
        using namespace nlohmann;
        auto options = node::get_options();

        return options;
    }

    void prepare() final {}

    void execute(const node_cfg_t&) final {}

    void complete() final {}

    node_type_t type() final { return node_type_t::invalid; }
};

std::shared_ptr<node> create_node(const std::string& id) { return std::make_shared<node_impl>(id); }

} // namespace miximus::nodes::dummy
