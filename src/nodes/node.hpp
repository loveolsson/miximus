#pragma once
#include "messages/types.hpp"
#include "nodes/node_config.hpp"
#include "nodes/node_type.hpp"

#include <nlohmann/json_fwd.hpp>

#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace miximus::nodes {

class node
{
  protected:
    enum class state_t
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

    const std::string id_;

    node(const std::string& id)
        : id_(id)
        , state_(state_t::uninitialized)
    {
    }

    virtual ~node() {}

  public:
    virtual bool           set_option(std::string_view option, const nlohmann::json&);
    virtual nlohmann::json get_options();
    virtual node_type_t    type() = 0;

    virtual void prepare()                  = 0;
    virtual void execute(const node_cfg_t&) = 0;
    virtual void complete()                 = 0;

    const std::string& id() const { return id_; }
};

std::shared_ptr<node> create_node(node_type_t type, const std::string& id, message::error_t& error);

} // namespace miximus::nodes