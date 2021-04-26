#pragma once

#include <concurrentqueue.h>
#include <nlohmann/json.hpp>
#include <websocketpp/common/asio.hpp>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>

#include <memory>
#include <set>
#include <string_view>

namespace miximus::web_server::detail {
class web_server_impl {
  typedef websocketpp::connection_hdl connection_hdl;
  typedef std::set<connection_hdl, std::owner_less<connection_hdl>> con_list;
  typedef websocketpp::server<websocketpp::config::asio> server;

  void on_http(connection_hdl hdl);
  void on_open(connection_hdl hdl);
  void on_close(connection_hdl hdl);

  websocketpp::lib::shared_ptr<websocketpp::lib::thread> run_thread_;

  server endpoint_;
  con_list connections_;

  moodycamel::ConcurrentQueue<nlohmann::json> incoming_messages_;

public:
  web_server_impl();
  ~web_server_impl();

  void start(std::string_view host, uint16_t port);
  void stop();
  void broadcast_message(nlohmann::json &&msg);
  int receive_messages(std::vector<nlohmann::json> &queue);
};
} // namespace miximus::web_server::detail