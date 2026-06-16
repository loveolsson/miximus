#pragma once
#include "web_server/detail/custom-logger.hpp"
#include <websocketpp/concurrency/basic.hpp>
#include <websocketpp/config/asio_no_tls.hpp>
#include <websocketpp/server.hpp>
#include <websocketpp/transport/asio/endpoint.hpp>

namespace miximus::web_server::detail {

// Custom server config based on bundled asio config
struct custom_config : public websocketpp::config::asio
{
    // Replace default stream logger with the custom logger
    typedef websocketpp::log::custom_logger<websocketpp::concurrency::basic, websocketpp::log::elevel> elog_type;
    typedef websocketpp::log::custom_logger<websocketpp::concurrency::basic, websocketpp::log::alevel> alog_type;

    // transport_config inherits type::alog_type/elog_type from the base config's 'type'
    // alias (which resolves to websocketpp::config::asio, not custom_config).  Because
    // write() is non-virtual the transport stores loggers as shared_ptr<basic<...>> and
    // dispatches to basic::write() — bypassing our filter.  Override both here so the
    // transport also uses our custom logger.
    struct transport_config : public websocketpp::config::asio::transport_config
    {
        typedef custom_config::elog_type elog_type;
        typedef custom_config::alog_type alog_type;
    };
    typedef websocketpp::transport::asio::endpoint<transport_config> transport_type;
};

} // namespace miximus::web_server::detail