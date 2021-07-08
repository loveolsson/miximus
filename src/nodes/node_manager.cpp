#include "nodes/node_manager.hpp"
#include "logger/logger.hpp"
#include "messages/payload.hpp"
#include "messages/templates.hpp"
#include "nodes/node.hpp"
#include "utils/bind.hpp"
#include "web_server/server.hpp"

namespace miximus {
using namespace nlohmann;
using namespace message;

node_manager::node_manager()
    : server_(nullptr)
{
}

void node_manager::make_server_subscriptions(web_server::server& server)
{
    server_ = &server;

    server.subscribe(topic_e::add_node, utils::bind(&node_manager::handle_add_node, this));
    server.subscribe(topic_e::remove_node, utils::bind(&node_manager::handle_remove_node, this));
    server.subscribe(topic_e::add_connection, utils::bind(&node_manager::handle_add_connection, this));
    server.subscribe(topic_e::remove_connection, utils::bind(&node_manager::handle_remove_connection, this));
    server.subscribe(topic_e::update_node, utils::bind(&node_manager::hande_update_node, this));
    server.subscribe(topic_e::config, utils::bind(&node_manager::handle_config, this));
}

nodes::node_cfg node_manager::clone_node_config()
{
    std::shared_lock lock(nodes_mutex_);

    auto clone = config_;
    return clone;
}

void node_manager::handle_add_node(json&& msg, int64_t client_id, const web_server::response_fn_t& cb)
{
    std::shared_lock lock(nodes_mutex_);

    auto token = get_token_from_payload(msg);

    try {
        json&       node_obj = msg["node"];
        std::string id       = node_obj["id"];
        std::string type     = node_obj["type"];

        if (config_.find_node(id) != nullptr) {
            return cb(create_error_base_payload(token, error_e::duplicate_id));
        }

        message::error_e error = message::error_e::no_error;
        auto             node  = create_node(type, error);

        if (error != message::error_e::no_error || !node) {
            return cb(create_error_base_payload(token, error));
        }

        auto options_it = node_obj.find("options");
        if (options_it != node_obj.end()) {
            auto& options = options_it.value();

            for (auto option = options.begin(); option != options.end(); ++option) {
                node->set_option(option.key(), option.value());
            }
        }

        config_.nodes.emplace(id, node);

        auto bcast_payload         = create_command_base_payload(topic_e::add_node);
        bcast_payload["origin_id"] = client_id;
        bcast_payload["node"]      = {
            {"id", id},
            {"type", type},
            {"options", node->get_options()},
        };

        cb(create_result_base_payload(token));

        if (server_ != nullptr) {
            server_->broadcast_message_sync(bcast_payload);
        }
    } catch (json::exception&) {
        return cb(create_error_base_payload(token, error_e::malformed_payload));
    }
}

void node_manager::handle_remove_node(json&& msg, int64_t, const web_server::response_fn_t& cb)
{
    std::shared_lock lock(nodes_mutex_);

    auto token = get_token_from_payload(msg);
}

void node_manager::hande_update_node(json&& msg, int64_t, const web_server::response_fn_t& cb)
{
    std::shared_lock lock(nodes_mutex_);
}

void node_manager::handle_add_connection(json&& msg, int64_t, const web_server::response_fn_t& cb)
{
    std::shared_lock lock(nodes_mutex_);
}

void node_manager::handle_remove_connection(json&& msg, int64_t, const web_server::response_fn_t& cb)
{
    std::shared_lock lock(nodes_mutex_);
}

void node_manager::handle_config(json&& msg, int64_t client_id, const web_server::response_fn_t& cb)
{
    (void)client_id;

    auto token    = get_token_from_payload(msg);
    auto response = create_result_base_payload(token);

    response["config"] = get_config();
    cb(std::move(response));
}

json node_manager::get_config()
{
    std::shared_lock lock(config_mutex_);

    auto nodes       = json::array();
    auto connections = json::array();

    for (const auto& [id, node] : config_.nodes) {
        nlohmann::json cfg{
            {"id", id},
            {"type", type_from_string(node->type())},
            {"options", node->get_options()},
        };

        nodes.emplace_back(std::move(cfg));
    }

    for (const auto& con : config_.connections) {
        nlohmann::json cfg{
            {"from_node", con.from_node},
            {"from_interface", con.from_interface},
            {"to_node", con.to_node},
            {"to_interface", con.to_interface},
        };

        connections.emplace_back(std::move(cfg));
    }

    return {
        {"nodes", std::move(nodes)},
        {"connections", std::move(connections)},
    };
}

std::shared_ptr<nodes::node> node_manager::create_node(const std::string& type, message::error_e& error)
{
    nodes::node_type_e t = nodes::type_from_string(type);
    return nodes::create_node(t, error);
}

} // namespace miximus