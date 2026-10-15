#include "app_state.hpp"
#include "logger/logger.hpp"
#include "node_manager.hpp"
#include "nodes/node.hpp"

#include <boost/asio/io_context.hpp>
#include <boost/asio/post.hpp>

namespace miximus::core {

error_e node_manager_s::handle_file_dialog(app_state_s* app, std::string_view id, std::string_view field)
{
    if (id.empty() || field.empty()) {
        return error_e::invalid_payload;
    }
    if (app == nullptr || app->file_dialog() == nullptr) {
        return error_e::unavailable;
    }
    std::optional<utils::file_dialog_request_s> request;
    {
        const std::unique_lock lock(nodes_mutex_);
        const auto             node = nodes_.find(id);
        if (node == nodes_.end()) {
            return error_e::not_found;
        }
        request = node->second.node->file_dialog_request(field, node->second.state);
    }
    if (!request) {
        return error_e::invalid_options;
    }
    // The application stops the dialog worker before the configuration executor
    // and manager. Only owned request values cross the asynchronous boundary.
    return app->file_dialog()->open(
        std::move(*request),
        [executor = app->cfg_executor(), this, id = std::string(id), field = std::string(field)](
            utils::file_dialog_result_s selected) {
            boost::asio::post(*executor, [this, id, field, selected = std::move(selected)] {
                try {
                    auto error = selected.error;
                    if (error == error_e::no_error && selected.path) {
                        error = handle_update_node(id,
                                                   {
                                                       {field, *selected.path}
                        })
                                    .error;
                    }
                    if (error != error_e::no_error && error != error_e::cancelled) {
                        getlog("app")->warn("File selection for {}.{} failed: {}", id, field, static_cast<int>(error));
                    }
                } catch (const std::exception& error) {
                    getlog("app")->error("File selection for {}.{} failed: {}", id, field, error.what());
                }
            });
        });
}

} // namespace miximus::core
