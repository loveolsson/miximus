#pragma once
#include "core/app_state_fwd.hpp"
#include "nodes/node_map_fwd.hpp"
#include "nodes/option_result.hpp"
#include "types/error.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

namespace miximus::nodes {

struct action_result_s;

class node_i
{
    friend class interface_i;

    void register_interface(const interface_i& iface);

  protected:
    interface_map_t interfaces_;
    std::string     id_;

    node_i()          = default;
    virtual ~node_i() = default;

  public:
    struct prepare_result_s
    {
        bool demands_execution{};
    };

    virtual std::string_view type() const = 0;

    /**
     * Called once on the config thread immediately after the node is constructed,
     * while the config lock is held. Do not record GPU work here. Use only for
     * lightweight one-time setup that does not require the render thread.
     */
    virtual void init(std::string_view id);

    // Transient custom action, called on the render thread before prepare with
    // this frame's configuration (possibly before this node's first prepare).
    // Validate payload before side effects. Never block or record GPU work;
    // schedule background work and report its progress through node status.
    // Returning success acknowledges handling/admission, not background completion.
    virtual action_result_s
    handle_action(core::app_state_s*, const node_state_s&, std::string_view, const nlohmann::json&);

    /**
     * Called every tick on the main/render thread. Update lifecycle state,
     * inspect options, and schedule explicit background work without blocking.
     */
    virtual void prepare(core::app_state_s*, const node_state_s&, prepare_result_s*) {};

    /**
     * Called once for every node in the demanded upstream closure after all
     * nodes have prepared and before any demanded node executes. Use this to
     * park frame-local work or initiate asynchronous work; do not wait for it.
     */
    virtual void submit(core::app_state_s*, const node_map_t&, const node_state_s&);

    /**
     * Called on the main thread with explicit GPU recording ownership. Invoked lazily
     * via dependency resolution — at most once per tick. May be called recursively
     * from within another node's execute() when resolving interface connections.
     * Resolve upstream targets before recording downstream operations.
     */
    virtual void execute(core::app_state_s*, const node_map_t&, const node_state_s&) = 0;

    /**
     * Called on the main thread after execution. GPU commands may still run.
     * Release CPU frame references; submitted GPU uses retain native storage.
     * Do not block; use workers for slow I/O and readback completion.
     */
    virtual void complete(core::app_state_s*) {}

    /**
     * Render-snapshot nodes are destroyed on the main thread. GPU allocations
     * retire after their actual submitted uses and external leases complete.
     */

    virtual nlohmann::json               get_default_options() const;
    virtual option_result_e              normalize_option(std::string_view name, nlohmann::json* value) const = 0;
    [[nodiscard]] static option_result_e normalize_common_option(std::string_view name, nlohmann::json* value);
    [[nodiscard]] set_options_result_s   set_options(nlohmann::json& state, const nlohmann::json& options) const;

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

} // namespace miximus::nodes
