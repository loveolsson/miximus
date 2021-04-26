#include "web_server/detail/web_server_impl.hpp"
#include "static_files/files.hpp"

namespace miximus::web_server::detail {

web_server_impl::web_server_impl() {
  endpoint_.clear_access_channels(websocketpp::log::alevel::all);
  endpoint_.set_access_channels(websocketpp::log::alevel::access_core);
  endpoint_.set_access_channels(websocketpp::log::alevel::app);

  // Initialize the Asio transport policy
  endpoint_.init_asio();

  // Bind the handlers we are using
  using websocketpp::lib::bind;
  using websocketpp::lib::placeholders::_1;
  endpoint_.set_open_handler(bind(&web_server_impl::on_open, this, _1));
  endpoint_.set_close_handler(bind(&web_server_impl::on_close, this, _1));
  endpoint_.set_http_handler(bind(&web_server_impl::on_http, this, _1));
}

web_server_impl::~web_server_impl() {}

void web_server_impl::on_http(connection_hdl hdl) {
  // Upgrade our connection handle to a full connection_ptr
  server::connection_ptr con = endpoint_.get_con_from_hdl(hdl);

  std::string_view resource = con->get_resource();

  endpoint_.get_alog().write(websocketpp::log::alevel::app,
                             std::string(resource));

  if (resource == "/") {
    resource = "/index.html";
  }

  auto &files = static_files::web_files;
  auto file_it = files.find(resource.substr(1));

  if (file_it != files.end()) {
    auto encoding = con->get_request_header("Accept-Encoding");
    if (encoding.find("gzip") != std::string::npos) {
      con->set_body(file_it->second.gzipped);
      con->replace_header("Content-Encoding", "gzip");
    } else {
      con->set_body(file_it->second.raw);
    }

    con->replace_header("Content-Type", std::string(file_it->second.mime));
    con->set_status(websocketpp::http::status_code::ok);
    return;
  }

  // 404 error
  std::stringstream ss;

  ss << "<!doctype html><html><head>"
     << "<title>Error 404 (Resource not found)</title><body>"
     << "<h1>Error 404</h1>"
     << "<p>The requested URL " << resource
     << " was not found on this server.</p>"
     << "</body></head></html>";

  con->set_body(ss.str());
  con->set_status(websocketpp::http::status_code::not_found);
}

void web_server_impl::on_open(connection_hdl hdl) { connections_.insert(hdl); }

void web_server_impl::on_close(connection_hdl hdl) { connections_.erase(hdl); }

void web_server_impl::start(std::string_view host, uint16_t port) {
  // listen on specified port
  endpoint_.listen(port);

  // Start the server accept loop
  endpoint_.start_accept();

  run_thread_.reset(new websocketpp::lib::thread(&server::run, &endpoint_));
}

void web_server_impl::stop() {
  std::cout << "Stopping server" << std::endl;

  endpoint_.get_io_service().post([&]() {
    std::error_code ec;
    endpoint_.stop_listening();
    if (ec) {
      std::cout << "Failed to stop listening" << std::endl;
    }

    for (auto it = connections_.begin(); it != connections_.end(); ++it) {
      websocketpp::lib::error_code ec;
      endpoint_.close(*it, websocketpp::close::status::going_away, "", ec);
      if (ec) {
        std::cout << "> Error closing connection "
                  << ": " << ec.message() << std::endl;
      }
    }
  });

  if (run_thread_ && run_thread_->joinable()) {
    run_thread_->join();
    run_thread_.reset();
  }
}

void web_server_impl::broadcast_message(nlohmann::json &&msg) {
  endpoint_.get_io_service().post([this, msg = std::move(msg)]() {
    auto serialized = msg.dump();

    for (auto &con : connections_) {
      endpoint_.send(con, serialized, websocketpp::frame::opcode::text);
    }
  });
}

int web_server_impl::receive_messages(std::vector<nlohmann::json> &queue) {
  nlohmann::json msg;
  int count = 0;

  while (incoming_messages_.try_dequeue(msg)) {
    queue.emplace_back(std::move(msg));
    count++;
  }

  return count;
}

} // namespace miximus::web_server::detail