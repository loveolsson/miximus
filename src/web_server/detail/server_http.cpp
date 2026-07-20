#include "headers.hpp"
#include "html.hpp"
#include "static_files/files.hpp"
#include "web_server/detail/server_impl.hpp"

#include <boost/url/parse.hpp>
#include <boost/url/url_view.hpp>

#include <format>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view API_V1_PREFIX = "/api/v1/";

} // namespace

namespace miximus::web_server::detail {

void web_server_impl::on_http(const con_hdl_t& hdl)
{
    using namespace websocketpp::log;

    websocketpp::lib::error_code error;
    auto                         connection = endpoint_.get_con_from_hdl(hdl, error);
    if (error) {
        return;
    }

    const auto& resource = connection->get_resource();
    const auto& method   = connection->get_request().get_method();

    const auto       result = boost::urls::parse_relative_ref(resource);
    const auto       parsed = result.has_value() ? *result : boost::urls::url_view{};
    std::string_view path   = parsed.encoded_path();
    if (path.empty()) {
        path = "/";
    }

    endpoint_.get_alog().write(alevel::http, std::string(path));

    if (path.starts_with(API_V1_PREFIX)) {
        handle_api_request(connection, method, path.substr(API_V1_PREFIX.length()));
        return;
    }

    serve_static_file(connection, path);
}

void web_server_impl::serve_static_file(const server_t::connection_ptr& connection, std::string_view path)
{
    using namespace websocketpp::http;

    std::string_view resource = path;
    if (resource == "/") {
        resource = "/index.html";
    }

    const auto& files = static_files::get_web_files();
    const auto* file  = files.get_file(resource.substr(1));

    if (file == nullptr) {
        connection->set_body(create_404_body(std::string(path)));
        connection->replace_header("Content-Type", "text/html;charset=UTF-8");
        connection->set_status(status_code::not_found);
        return;
    }

    const auto etag          = std::format("\"{}\"", file->etag);
    const auto if_none_match = connection->get_request_header("If-None-Match");
    if (!if_none_match.empty() && if_none_match == etag) {
        connection->set_status(status_code::not_modified);
        return;
    }

    if (accept_encoding_has_gzip(connection->get_request_header("Accept-Encoding"))) {
        connection->replace_header("Content-Encoding", "gzip");
        connection->set_body(std::string(reinterpret_cast<const char*>(file->gzipped.data()), file->gzipped.size()));
    } else {
        connection->set_body(file->unzip());
    }

    connection->replace_header("Content-Type", std::string(file->mime));
    connection->replace_header("ETag", etag);
    connection->replace_header("Cache-Control", "no-cache");
    connection->set_status(status_code::ok);
}

} // namespace miximus::web_server::detail
