#include "nodes/node_manager.hpp"
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
    server.subscribe(topic_t::add_node, utils::bind(&node_manager::handle_add_node, this));
    server.subscribe(topic_t::remove_node, utils::bind(&node_manager::handle_remove_node, this));
    server.subscribe(topic_t::add_connection, utils::bind(&node_manager::handle_add_connection, this));
    server.subscribe(topic_t::remove_connection, utils::bind(&node_manager::handle_remove_connection, this));
    server.subscribe(topic_t::update_node, utils::bind(&node_manager::hande_update_node, this));
    server.subscribe(topic_t::config, utils::bind(&node_manager::handle_config, this));
}

void node_manager::handle_add_node(json&& msg, int64_t, web_server::response_fn_t cb)
{
    auto token = get_token_from_payload(msg);

    try {
        json&       node_obj = msg["node"];
        std::string id       = node_obj["id"];
        std::string type     = node_obj["type"];

        if (find_node(id)) {
            return cb(create_error_base_payload(token, error_t::duplicate_id));
        }

        // if (!handle_add_node(type, id)) {
        //     return respond_error(client_id, token, error_t::invalid_type);
        // }

        auto options = node_obj.find("options");
        if (options != node_obj.end()) {
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

void node_manager::handle_config(json&& msg, int64_t, web_server::response_fn_t cb)
{
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

std::shared_ptr<node> node_manager::create_node(const std::string& type, const std::string& id)
{
    node_type_t t = node_type_from_string(type);
    if (t == node_type_t::invalid) {
        return nullptr;
    }

    return node_factory::create_node(t, id);
}

} // namespace miximus