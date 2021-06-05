#include "nodes/node_manager.hpp"
#include "logger/logger.hpp"
#include "messages/payload.hpp"
#include "messages/templates.hpp"
#include "utils/bind.hpp"
#include "web_server/web_server.hpp"

namespace miximus {
using namespace nlohmann;
using namespace message;

node_manager::node_manager() {}

node_manager::~node_manager() {}

void node_manager::make_server_subscriptions(web_server::web_server& server)
{
    server_ = &server;

    server.subscribe(topic_t::add_node, utils::bind(&node_manager::handle_add_node, this));
    server.subscribe(topic_t::remove_node, utils::bind(&node_manager::handle_remove_node, this));
    server.subscribe(topic_t::add_connection, utils::bind(&node_manager::handle_add_connection, this));
    server.subscribe(topic_t::remove_connection, utils::bind(&node_manager::handle_remove_connection, this));
    server.subscribe(topic_t::update_node, utils::bind(&node_manager::hande_update_node, this));
    server.subscribe(topic_t::config, utils::bind(&node_manager::handle_config, this));
}

node_manager::node_map_t node_manager::clone_node_map()
{
    std::shared_lock lock(nodes_mutex_);
    node_map_t       copy = nodes_;
    return copy;
}

void node_manager::handle_add_node(json&& msg, int64_t client_id, web_server::response_fn_t cb)
{
    spdlog::get("application")->info(msg.dump());

    auto token = get_token_from_payload(msg);

    try {
        json&       node_obj = msg["node"];
        std::string id       = node_obj["id"];
        std::string type     = node_obj["type"];

        if (find_node(id)) {
            return cb(create_error_base_payload(token, error_t::duplicate_id));
        }

        message::error_t error = message::error_t::no_error;
        auto             node  = create_node(type, id, error);

        if (error != message::error_t::no_error || !node) {
            return cb(create_error_base_payload(token, error));
        }

        auto bcast_payload         = create_command_base_payload(topic_t::add_node);
        bcast_payload["origin_id"] = client_id;
        bcast_payload["node"]      = json{
            {"id", id},
            {"type", type},
            {"options", json::object()},
        };

        auto options_it = node_obj.find("options");
        if (options_it != node_obj.end()) {
            auto& options = options_it.value();

            for (auto option = options.begin(); option != options.end(); ++option) {
                if (node->set_option(option.key(), option.value())) {
                    bcast_payload["node"]["options"][option.key()] = option.value();
                }
            }
        }

        {
            std::unique_lock lock(nodes_mutex_);
            nodes_.emplace(id, node);
        }

        if (server_) {
            server_->broadcast_message_sync(bcast_payload);
        }
    } catch (json::exception&) {
        return cb(create_error_base_payload(token, error_t::malformed_payload));
    }

    cb(create_result_base_payload(token));
}

void node_manager::handle_remove_node(json&& msg, int64_t, web_server::response_fn_t cb) {}

void node_manager::hande_update_node(json&& msg, int64_t, web_server::response_fn_t cb) {}

void node_manager::handle_add_connection(json&& msg, int64_t, web_server::response_fn_t cb) {}

void node_manager::handle_remove_connection(json&& msg, int64_t, web_server::response_fn_t cb) {}

void node_manager::handle_config(json&& msg, int64_t client_id, web_server::response_fn_t cb)
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

    for (const auto& [id, cfg] : node_config_) {
        nodes.emplace_back(cfg);
    }

    for (const auto& [id, con] : con_config_) {
        connections.emplace_back(con);
    }

    return {
        {"nodes", std::move(nodes)},
        {"connections", std::move(connections)},
    };
}

std::shared_ptr<nodes::node>
node_manager::create_node(const std::string& type, const std::string& id, message::error_t& error)
{
    nodes::node_type_t t = nodes::type_from_string(type);
    return nodes::create_node(t, id, error);
}

} // namespace miximus