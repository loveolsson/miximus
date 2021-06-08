#pragma once
#include "messages/types.hpp"
#include "nodes/node_type.hpp"
#include "nodes/option.hpp"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <string_view>
#include <unordered_map>

namespace miximus::nodes {

class interface;
class node_cfg_t;

class node
{
    typedef std::unordered_map<std::string_view, interface*> interface_map_t;
    typedef std::unordered_map<std::string_view, option*>    option_map_t;

    option_position opt_position_;
    option_name     opt_name_;

  protected:
    enum class state_e
    {
        uninitialized,
        prepared,
        ready,
        complete,
    } state_;

    struct
    {
        double x, y;
    } position_;

    interface_map_t interfaces_;
    option_map_t    options_;

    node();
    virtual ~node() {}

  public:
    virtual node_type_e type()                     = 0;
    virtual void        prepare()                  = 0;
    virtual void        execute(const node_cfg_t&) = 0;
    virtual void        complete();

    bool           set_option(std::string_view option, const nlohmann::json&);
    nlohmann::json get_options();

    interface* find_interface(std::string_view name);
    interface* get_prepared_interface(const node_cfg_t& cfg, std::string_view name);
};

inline interface* node::find_interface(std::string_view name)
{
    auto it = interfaces_.find(name);
    if (it != interfaces_.end()) {
        return it->second;
    }
    return nullptr;
}

std::shared_ptr<node> create_node(node_type_e type, message::error_t& error);

} // namespace miximus::nodes