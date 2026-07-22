#pragma once
#include "core/app_state_fwd.hpp"
#include "nodes/node_map_fwd.hpp"
#include "nodes/option_result.hpp"
#include "types/error.hpp"

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <string_view>

namespace miximus::nodes {

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
     * while the config lock is held. No GL context is available. Use only for
     * lightweight one-time setup that does not require the render thread.
     */
    virtual void init(std::string_view id);

    /**
     * Called every tick on the main thread. The main GL context is current.
     * Nodes may scope additional current contexts (e.g. for worker threads) with
     * gpu::context_scope_s. Because the context stack makes re-entering the
     * already-current context essentially free, nodes can create a scope
     * unconditionally when they need the context.
     */
    virtual void prepare(core::app_state_s*, const node_state_s&, prepare_result_s*) {};

    /**
     * Called once for every node in the demanded upstream closure after all
     * nodes have prepared and before any demanded node executes. Use this to
     * park frame-local work or initiate asynchronous work; do not wait for it.
     */
    virtual void submit(core::app_state_s*, const node_map_t&, const node_state_s&) {}

    /**
     * Called on the main thread with the main GL context current. Invoked lazily
     * via dependency resolution — at most once per tick. May be called recursively
     * from within another node's execute() when resolving interface connections.
     * Nodes may push/pop additional contexts onto the stack.
     */
    virtual void execute(core::app_state_s*, const node_map_t&, const node_state_s&) = 0;

    /**
     * Called on the main thread with the main GL context current, AFTER
     * gpu::context_s::finish() — all GPU commands submitted during execute() are
     * guaranteed complete. Use for readback results or posting data to output
     * queues. Must not block the main thread (use worker threads for slow I/O).
     */
    virtual void complete(core::app_state_s*) {}

    /**
     * Destructor — always called on the main thread with the main GL context
     * current (the context scope begins before the update block that may trigger
     * destruction, and clear_nodes() also establishes a context scope first).
     * GL cleanup is safe from the destructor.
     */

    virtual nlohmann::json  get_default_options() const;
    virtual option_result_e normalize_option(std::string_view name, nlohmann::json* value) const = 0;
    static option_result_e  normalize_common_option(std::string_view name, nlohmann::json* value);
    set_options_result_s    set_options(nlohmann::json& state, const nlohmann::json& options) const;

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
