#pragma once
#include "application/app_state.hpp"
#include "types/error.hpp"
#include "types/node_map.hpp"
#include "types/node_type.hpp"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <string_view>
#include <unordered_map>

namespace miximus::nodes {

class interface_i;

class node_i
{
    using interface_map_t = std::unordered_map<std::string_view, interface_i*>;

  protected:
    interface_map_t interfaces_;

    node_i()          = default;
    virtual ~node_i() = default;

  public:
    virtual node_type_e type() const                                                  = 0;
    virtual bool        prepare(app_state_s&, const node_state_s&)                    = 0;
    virtual void        execute(app_state_s&, const node_map_t&, const node_state_s&) = 0;
    virtual void        complete(){};

    virtual nlohmann::json get_default_options() const { return nlohmann::json::object(); }
    virtual bool           check_option(std::string_view name, const nlohmann::json& value) const = 0;

    const interface_map_t& get_interfaces() const { return interfaces_; }
    interface_i*           find_interface(std::string_view name);
};

std::shared_ptr<node_i> create_node(node_type_e type, error_e& error);

} // namespace miximus::nodes