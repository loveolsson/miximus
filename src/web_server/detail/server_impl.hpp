#pragma once
#include "static_files/files.hpp"
#include "types/error.hpp"
#include "url_parser.hpp"
#include "web_server/detail/custom-config.hpp"
#include "web_server/payload.hpp"
#include "web_server/server.hpp"
#include "websocket_connection.hpp"

#include <websocketpp/common/asio.hpp>

#include <memory>
#include <set>
#include <string_view>

namespace miximus::web_server::detail {
class web_server_impl : public server_s
{
    using server_t       = websocketpp::server<custom_config>;
    using con_hdl_t      = websocketpp::connection_hdl;
    using con_map_t      = std::map<con_hdl_t, websocket_connection, std::owner_less<con_hdl_t>>;
    using con_by_id_t    = std::map<int64_t, con_hdl_t>;
    using con_set_t      = std::set<con_hdl_t, std::owner_less<con_hdl_t>>;
    using con_by_topic_t = std::array<con_set_t, enum_count<topic_e>()>; // Includes invalid
    using sub_by_topic_t = std::array<callback_t, enum_count<topic_e>()>;
    using msg_ptr_t      = server_t::message_ptr;

    void terminate_and_log(const con_hdl_t& hdl, const std::string& msg);

    void on_http(const con_hdl_t& hdl);
    void handle_api_request(server_t::connection_ptr con,
                            const std::string&       method,
                            const url_parser&        parser);
    void handle_api_v1_get_config(server_t::connection_ptr con);
    void handle_api_v1_post_control(server_t::connection_ptr con);
    error_e handle_user_command(nlohmann::json&& doc, int64_t connection_id);
    void on_message(const con_hdl_t& hdl, const msg_ptr_t& msg);
    void on_open(const con_hdl_t& hdl);
    void on_close(const con_hdl_t& hdl);

    void send(const con_hdl_t& hdl, const std::string&);
    void send(const con_hdl_t& hdl, const nlohmann::json&);

    server_t        endpoint_;
    con_map_t       connections_;
    con_by_id_t     connections_by_id_;
    con_by_topic_t  connections_by_topic_;
    sub_by_topic_t  subscription_by_topic_;
    config_getter_t config_getter_;

    int64_t next_connection_id_ = 0;

  public:
    web_server_impl();
    ~web_server_impl() = default;

    virtual void subscribe(topic_e topic, const callback_t& callback) override;
    virtual void set_config_getter(const config_getter_t& getter) override;
    virtual void start(uint16_t port, boost::asio::io_service* service) override;
    void         stop() final;

    void send_message(const nlohmann::json& msg, int64_t connection_id) final;
    void send_message_sync(const nlohmann::json& msg, int64_t connection_id) final;
    void send_message_sync(const std::string& msg, int64_t connection_id);

    void broadcast_message(const nlohmann::json& msg) final;
    void broadcast_message_sync(const nlohmann::json& msg) final;
    void broadcast_message_sync(topic_e topic, const std::string& msg);
};
} // namespace miximus::web_server::detail