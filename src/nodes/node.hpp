#pragma once
#include "core/app_state_fwd.hpp"
#include "nodes/node_map.hpp"
#include "types/error.hpp"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <map>
#include <memory>
#include <string_view>

namespace miximus::nodes {

class interface_i;

class node_i
{
    using interface_map_t = std::map<std::string_view, const interface_i*>;

  protected:
    interface_map_t interfaces_;

    node_i()          = default;
    virtual ~node_i() = default;

  public:
    struct traits_s
    {
        bool must_run : 1;
        bool wait_for_sync : 1;
    };

    virtual std::string_view type() const                                                        = 0;
    virtual void             prepare(core::app_state_s*, const node_state_s&, traits_s*)         = 0;
    virtual void             execute(core::app_state_s*, const node_map_t&, const node_state_s&) = 0;
    virtual void             complete(core::app_state_s*) {}

    virtual nlohmann::json get_default_options() const { return nlohmann::json::object(); }
    virtual bool           test_option(std::string_view name, nlohmann::json* value) const = 0;

    const interface_map_t& get_interfaces() const { return interfaces_; }
    const interface_i*     find_interface(std::string_view name) const;
};

using constructor_t     = std::function<std::shared_ptr<node_i>()>;
using constructor_map_t = std::map<std::string_view, constructor_t>;

} // namespace miximus::nodes