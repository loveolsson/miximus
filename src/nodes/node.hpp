#pragma once
#include "core/app_state_fwd.hpp"
#include "nodes/node_map.hpp"
#include "types/error.hpp"

#include <nlohmann/json_fwd.hpp>

#include <concepts>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace miximus::core {
class node_status_registry_s;
}

namespace miximus::nodes {

class node_i
{
  protected:
    interface_map_t interfaces_;
    std::string     id_;

    node_i()          = default;
    virtual ~node_i() = default;

    void register_interface(const interface_i* iface);

  public:
    struct traits_s
    {
        bool must_run;
        bool wait_for_sync;
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
    virtual void prepare(core::app_state_s*, const node_state_s&, traits_s*) {};

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

using constructor_t = std::function<std::shared_ptr<node_i>()>;

struct node_migration_s
{
    std::function<void(nlohmann::json&)> migrate_options;
    std::function<void(std::string&)>    migrate_input_interface;
    std::function<void(std::string&)>    migrate_output_interface;
};

struct node_definition_s
{
    constructor_t                 constructor;
    std::vector<node_migration_s> migrations;

    [[nodiscard]] uint32_t schema_version() const noexcept { return static_cast<uint32_t>(migrations.size()) + 1; }

    template <typename Callable>
        requires std::constructible_from<constructor_t, Callable>
    node_definition_s(Callable&& callable)
        : constructor(std::forward<Callable>(callable))
    {
    }

    node_definition_s(constructor_t constructor_, std::vector<node_migration_s> migrations_)
        : constructor(std::move(constructor_))
        , migrations(std::move(migrations_))
    {
    }
};

using node_definition_map_t = std::map<std::string_view, node_definition_s>;

} // namespace miximus::nodes
