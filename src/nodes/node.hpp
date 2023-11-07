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

class node_i
{
  protected:
    interface_map_t interfaces_;

    node_i()          = default;
    virtual ~node_i() = default;

    void register_interface(const interface_i* iface);

  public:
    struct traits_s
    {
        bool must_run;
        bool wait_for_sync;
    };

    virtual std::string_view type() const                                                        = 0;
    virtual void             prepare(core::app_state_s*, const node_state_s&, traits_s*)         = 0;
    virtual void             execute(core::app_state_s*, const node_map_t&, const node_state_s&) = 0;
    virtual void             complete(core::app_state_s*) {}

    virtual nlohmann::json get_default_options() const { return nlohmann::json::object(); }
    virtual bool           test_option(std::string_view name, nlohmann::json* value) const = 0;
    static bool            is_valid_common_option(std::string_view name, nlohmann::json* value);
    error_e                set_options(nlohmann::json& state, const nlohmann::json& options) const;

    const interface_map_t& get_interfaces() const { return interfaces_; }
    const interface_i*     find_interface(std::string_view name) const;
};

inline const interface_i* node_i::find_interface(std::string_view name) const
{
    auto it = interfaces_.find(name);
    if (it != interfaces_.end()) {
        return it->second;
    }
    return nullptr;
}

using constructor_t     = std::function<std::shared_ptr<node_i>()>;
using constructor_map_t = std::map<std::string_view, constructor_t>;

} // namespace miximus::nodes