#pragma once
#include "nodes/node_type.hpp"
#include "nodes/option.hpp"
#include "types/error.hpp"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <string_view>
#include <unordered_map>

namespace miximus::nodes {

class interface;
class node_cfg;

class node
{
    typedef std::unordered_map<std::string_view, interface*> interface_map_t;
    typedef std::unordered_map<std::string_view, option*>    option_map_t;

    option_position opt_position_;
    option_name     opt_name_;

  protected:
    interface_map_t interfaces_;
    option_map_t    options_;

    node();
    virtual ~node() = default;

  public:
    virtual node_type_e type()                   = 0;
    virtual void        prepare()                = 0;
    virtual void        execute(const node_cfg&) = 0;
    virtual void        complete();

    bool           set_option(std::string_view option, const nlohmann::json&);
    nlohmann::json get_options();
    nlohmann::json get_option(std::string_view option);

    const interface_map_t& get_interfaces() const { return interfaces_; }
    interface*             find_interface(std::string_view name);

    // NOTE(Love): get_prepared_interface needs to be virtual to link on MSVC and I have no idea why
    virtual interface* get_prepared_interface(const node_cfg& cfg, std::string_view name);
};

std::shared_ptr<node> create_node(node_type_e type, error_e& error);

} // namespace miximus::nodes