#include "browser_inputs.hpp"
#include "browser_options.hpp"
#include "core/app_state.hpp"
#include "core/node_status_registry.hpp"
#include "nodes/action.hpp"
#include "nodes/interface.hpp"
#include "nodes/node.hpp"
#include "nodes/node_map.hpp"
#include "types/node_status_json.hpp"

namespace miximus::nodes::cef {

namespace {
class unavailable_browser_s final : public node_i
{
    output_interface_s<const gpu::texture_s*> texture_{*this, "tex"};
    browser_inputs_s                          inputs_{*this};

  public:
    action_result_s handle_action(core::app_state_s* /* app */,
                                  const node_state_s& /* state */,
                                  std::string_view name,
                                  const nlohmann::json& /* payload */) final
    {
        return name == "reload"
                   ? action_result_s{.error   = error_e::unavailable,
                                     .message = "CEF support is not enabled in this build"}
                   : action_result_s{.error = error_e::unsupported_action, .message = "Unknown browser action"};
    }

    void prepare(core::app_state_s* app, const node_state_s& state, prepare_result_s* /* result */) final
    {
        const bool stopped = !state.get_option<bool>("enabled") || state.get_option<std::string>("url").empty();
        app->status_registry()->write(status_handle_, status::connected_status_s{.connected = false});
        app->status_registry()->write(
            status_handle_,
            status::cef_browser_status_s{.cef_state        = stopped ? cef_state_e::stopped : cef_state_e::unavailable,
                                         .cef_error        = stopped ? "" : app->cef_error(),
                                         .cef_inputs_error = {}});
    }
    void execute(core::app_state_s* /* app */, const node_map_t& /* nodes */, const node_state_s& /* state */) final
    {
        texture_.set_value(nullptr);
    }
    nlohmann::json  get_default_options() const final { return browser_default_options(); }
    option_result_e normalize_option(std::string_view name, nlohmann::json* value) const final
    {
        return normalize_browser_option(name, value);
    }
    std::string_view type() const final { return "cef_browser"; }
};
} // namespace
std::shared_ptr<node_i> create_browser_node() { return std::make_shared<unavailable_browser_s>(); }
} // namespace miximus::nodes::cef
