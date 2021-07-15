#pragma once
#include "types/error.hpp"
#include "types/node_map.hpp"
#include "types/node_type.hpp"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <string_view>
#include <unordered_map>

namespace miximus::nodes {

class interface_i;
class node_cfg;

class node_i
{
    typedef std::unordered_map<std::string_view, interface_i*> interface_map_t;

  protected:
    interface_map_t interfaces_;

    node_i()          = default;
    virtual ~node_i() = default;

  public:
    virtual node_type_e type()                                  = 0;
    virtual bool        prepare(const node_state&)              = 0;
    virtual void        execute(node_map_t&, const node_state&) = 0;
    virtual void        complete();

    virtual nlohmann::json get_default_options() { return nlohmann::json::object(); }
    virtual bool           check_option(std::string_view name, const nlohmann::json& value) = 0;

    const interface_map_t& get_interfaces() const { return interfaces_; }
    interface_i*           find_interface(std::string_view name);
};

std::shared_ptr<node_i> create_node(node_type_e type, error_e& error);

} // namespace miximus::nodes