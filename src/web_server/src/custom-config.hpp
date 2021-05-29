#pragma once
#include "./custom-logger.hpp"
#include <websocketpp/concurrency/basic.hpp>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/extensions/permessage_deflate/enabled.hpp>
#include <websocketpp/logger/syslog.hpp>
#include <websocketpp/server.hpp>

namespace miximus::web_server::detail {

// Custom server config based on bundled asio config
struct custom_config : public websocketpp::config::asio
{
    // Replace default stream logger with the custom logger
    typedef websocketpp::log::custom_logger<websocketpp::concurrency::basic, websocketpp::log::elevel> elog_type;
    typedef websocketpp::log::custom_logger<websocketpp::concurrency::basic, websocketpp::log::alevel> alog_type;
};

} // namespace miximus::web_server::detail