#pragma once
#include "core/app_state.hpp"
#include "nodes/node_map.hpp"
#include "types/error.hpp"

#include <nlohmann/json_fwd.hpp>

#include <memory>
#include <string_view>
#include <unordered_map>

namespace miximus::nodes {

class interface_i;

class node_i
{
    using interface_map_t = std::unordered_map<std::string_view, const interface_i*>;

  public:
    enum class type_e
    {
        invalid  = -1,
        math_i64 = 0,
        math_f64,
        math_vec2,
        decklink_producer,
        decklink_consumer,
        _count,
    };

  protected:
    interface_map_t interfaces_;

    node_i()          = default;
    virtual ~node_i() = default;

  public:
    virtual type_e type() const                                                        = 0;
    virtual bool   prepare(core::app_state_s&, const node_state_s&)                    = 0;
    virtual void   execute(core::app_state_s&, const node_map_t&, const node_state_s&) = 0;
    virtual void   complete(){};

    virtual nlohmann::json get_default_options() const { return nlohmann::json::object(); }
    virtual bool           test_option(std::string_view name, const nlohmann::json& value) const = 0;

    const interface_map_t& get_interfaces() const { return interfaces_; }
    const interface_i*     find_interface(std::string_view name) const;

    static type_e           type_from_string(std::string_view type);
    static std::string_view type_to_string(type_e type);
};

std::shared_ptr<node_i> create_node(node_i::type_e type, error_e& error);

} // namespace miximus::nodes