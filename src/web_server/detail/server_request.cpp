#include "web_server/detail/server_impl.hpp"

#include <boost/url/parse.hpp>

namespace miximus::web_server::detail {
namespace {
boost::asio::ip::address normalized(boost::asio::ip::address address)
{
    if (address.is_v6() && address.to_v6().is_v4_mapped()) {
        return boost::asio::ip::make_address_v4(boost::asio::ip::v4_mapped, address.to_v6());
    }
    return address;
}
} // namespace

bool web_server_impl::is_request_local(const server_t::connection_ptr& con)
{
    // Forwarded requests cannot establish whether the client is on this host.
    if (!con->get_request_header("Forwarded").empty() || !con->get_request_header("X-Forwarded-For").empty() ||
        !con->get_request_header("X-Forwarded-Host").empty()) {
        return false;
    }
    boost::system::error_code error;
    const auto                peer = con->get_raw_socket().remote_endpoint(error);
    if (error) {
        return false;
    }
    const auto local = con->get_raw_socket().local_endpoint(error);
    if (error) {
        return false;
    }
    const auto remote_address = normalized(peer.address());
    const auto local_address  = normalized(local.address());
    if (!remote_address.is_loopback() && remote_address != local_address) {
        return false;
    }
    const auto expected_origin = std::string("http://") + con->get_request_header("Host");
    const auto url             = boost::urls::parse_uri(expected_origin);
    if (!url || url->has_userinfo() || url->has_query() || url->has_fragment() || !url->encoded_path().empty()) {
        return false;
    }
    // Restrict Host as well as Origin, including protection against DNS rebinding.
    const auto host    = url->host_address();
    const auto address = boost::asio::ip::make_address(host, error);
    if (error ? host != "localhost" || !local_address.is_loopback() : normalized(address) != local_address) {
        return false;
    }
    const auto& origin = con->get_request_header("Origin");
    if (!origin.empty() && origin != expected_origin) {
        return false;
    }
    return true;
}

} // namespace miximus::web_server::detail
