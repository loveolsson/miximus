#pragma once

#include "messages/payload.hpp"
#include "web_server/web_server.hpp"
#include "websocket_connection.hpp"

#include <websocketpp/common/asio.hpp>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <memory>
#include <set>
#include <string_view>

namespace miximus::web_server::detail {
class web_server_impl
{
    typedef websocketpp::server<websocketpp::config::asio>                                  server;
    typedef websocketpp::connection_hdl                                                     connection_hdl;
    typedef std::map<connection_hdl, websocket_connection, std::owner_less<connection_hdl>> con_list_t;
    typedef std::map<int64_t, connection_hdl>                                               con_by_id_t;
    typedef std::set<connection_hdl, std::owner_less<connection_hdl>>                       con_set_t;
    typedef std::array<con_set_t, static_cast<int>(message::topic_t::_count)>               con_by_topic_t;
    typedef std::array<callback_t, static_cast<int>(message::topic_t::_count)>              subscription_list_t;
    typedef server::message_ptr                                                             message_ptr;

    void terminate_and_log(connection_hdl hdl, const std::string& msg);

    void on_http(connection_hdl hdl);
    void on_message(connection_hdl hdl, message_ptr msg);
    void on_open(connection_hdl hdl);
    void on_close(connection_hdl hdl);

    void send(connection_hdl hdl, const std::string&);
    void send(connection_hdl hdl, const nlohmann::json&);

    server                       endpoint_;
    std::unique_ptr<std::thread> run_thread_;

    con_list_t          connections_;
    con_by_id_t         connections_by_id_;
    con_by_topic_t      connections_by_topic_;
    subscription_list_t subscriptions_;

  public:
    web_server_impl();
    ~web_server_impl();

    void subscribe(message::topic_t topic, callback_t callback);
    void start(uint16_t port);
    void stop();

    void send_message(const nlohmann::json& msg, int64_t connection_id);
    void send_message_sync(const nlohmann::json& msg, int64_t connection_id);
    void send_message_sync(const std::string& msg, int64_t connection_id);

    void broadcast_message(const nlohmann::json& msg);
    void broadcast_message_sync(const nlohmann::json& msg);
    void broadcast_message_sync(message::topic_t topic, const std::string& msg);
};
} // namespace miximus::web_server::detail