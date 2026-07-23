#pragma once
#include "types/error.hpp"
#include "utils/lookup.hpp"
#include "web_server/detail/custom-config.hpp"
#include "web_server/server.hpp"
#include "websocket_connection.hpp"

#include <websocketpp/common/asio.hpp>

#include <array>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace miximus::web_server::detail {
class web_server_impl : public server_s
{
    using server_t       = websocketpp::server<custom_config>;
    using con_hdl_t      = websocketpp::connection_hdl;
    using con_map_t      = std::map<con_hdl_t, websocket_connection, std::owner_less<con_hdl_t>>;
    using con_by_id_t    = std::map<int64_t, con_hdl_t>;
    using con_set_t      = std::set<con_hdl_t, std::owner_less<con_hdl_t>>;
    using con_by_topic_t = std::array<con_set_t, enum_count<topic_e>()>;
    using sub_by_topic_t = std::array<callback_t, enum_count<topic_e>()>;
    using msg_ptr_t      = server_t::message_ptr;

    void terminate_and_log(const con_hdl_t& hdl, const std::string& message);

    void        on_http(const con_hdl_t& hdl);
    static void serve_static_file(const server_t::connection_ptr& con, std::string_view path);
    void handle_api_request(const server_t::connection_ptr& con, const std::string& method, std::string_view api_path);
    void handle_api_v1_get_config(const server_t::connection_ptr& con) const;
    void handle_api_v1_get_node(const server_t::connection_ptr& con, std::string_view encoded_id) const;
    void handle_api_v1_get_node_status(const server_t::connection_ptr& con, std::string_view encoded_id) const;
    void handle_api_v1_post_control(const server_t::connection_ptr& con);
    error_e handle_user_command(nlohmann::json&& doc, int64_t connection_id);
    void    on_message(const con_hdl_t& hdl, const msg_ptr_t& msg);
    void    on_open(const con_hdl_t& hdl);
    void    on_fail(const con_hdl_t& hdl);
    void    on_close(const con_hdl_t& hdl);

    void send(const con_hdl_t& hdl, const std::string&);
    void send(const con_hdl_t& hdl, const nlohmann::json&);

    callback_t& get_subscription_by_topic(topic_e t)
    {
        return subscription_by_topic_[enum_index(t)];
    } // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)
    con_set_t& get_connections_by_topic(topic_e t)
    {
        return connections_by_topic_[enum_index(t)];
    } // NOLINT(cppcoreguidelines-pro-bounds-constant-array-index)

    server_t         endpoint_;
    con_map_t        connections_;
    con_by_id_t      connections_by_id_;
    con_by_topic_t   connections_by_topic_;
    sub_by_topic_t   subscription_by_topic_;
    config_getters_t config_getters_;

    int64_t next_connection_id_ = 0;
    bool    started_            = false;

    std::optional<std::promise<void>> stop_promise_;

  public:
    web_server_impl();
    ~web_server_impl() = default;

    void subscribe(topic_e topic, const callback_t& callback) final;
    void set_config_getters(const config_getters_t& getters) final;
    void start(uint16_t port, boost::asio::io_context* service) final;
    void stop() final;

    void send_message(const nlohmann::json& msg, int64_t connection_id) final;
    void send_message_sync(const nlohmann::json& msg, int64_t connection_id) final;
    void send_message_sync(const std::string& msg, int64_t connection_id);

    void broadcast_message(const nlohmann::json& msg) final;
    void broadcast_message_sync(const nlohmann::json& msg) final;
    void broadcast_message_sync(topic_e topic, const std::string& msg);
};
} // namespace miximus::web_server::detail
