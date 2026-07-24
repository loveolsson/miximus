#include "headers.hpp"
#include "html.hpp"
#include "static_files/files.hpp"
#include "web_server/detail/path.hpp"
#include "web_server/detail/server_impl.hpp"

#include <boost/url/parse.hpp>
#include <boost/url/segments_view.hpp>
#include <boost/url/url_view.hpp>

#include <exception>
#include <format>
#include <string>
#include <string_view>

namespace {

constexpr std::string_view HTTP_GET     = "GET";
constexpr std::string_view HTTP_HEAD    = "HEAD";
constexpr std::string_view HTTP_OPTIONS = "OPTIONS";

template <typename Connection>
void suppress_head_body(const Connection& connection, std::string_view method)
{
    if (method != HTTP_HEAD) {
        return;
    }

    const auto content_length = connection->get_response_header("Content-Length");
    connection->set_body("");
    if (!content_length.empty()) {
        connection->replace_header("Content-Length", content_length);
    }
}

std::string make_decoded_path(boost::urls::segments_view segments)
{
    std::string path;
    path.reserve(segments.buffer().size());

    if (segments.is_absolute()) {
        path.push_back('/');
    }

    bool first_segment = true;
    for (auto segment = segments.begin(); segment != segments.end(); ++segment) {
        if (!first_segment) {
            path.push_back('/');
        }
        path += *segment;
        first_segment = false;
    }

    return path.empty() ? "/" : path;
}

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
    connection->replace_header("Date", make_http_date());

    try {
        const auto result = boost::urls::parse_relative_ref(resource);
        if (!result.has_value()) {
            endpoint_.get_alog().write(alevel::http, resource);
            connection->replace_header("Content-Type", "text/plain;charset=UTF-8");
            connection->set_body("Malformed request target");
            connection->set_status(websocketpp::http::status_code::bad_request);
            connection->replace_header("Cache-Control", "no-store");
            suppress_head_body(connection, method);
            return;
        }

        const auto parsed = *result;
        const auto path   = parsed.segments();

        endpoint_.get_alog().write(alevel::http, std::string(parsed.encoded_path()));

        if (path_starts_with(path, {"api"})) {
            handle_api_request(connection, method, consume_segments(path, 1));
            suppress_head_body(connection, method);
            return;
        }

        serve_static_file(connection, method, path);
        suppress_head_body(connection, method);
    } catch (const std::exception& error) {
        endpoint_.get_elog().write(websocketpp::log::elevel::rerror,
                                   std::format("Failed to handle HTTP request: {}", error.what()));
        connection->remove_header("Content-Encoding");
        connection->remove_header("ETag");
        connection->remove_header("Vary");
        connection->replace_header("Cache-Control", "no-store");
        connection->replace_header("Content-Type", "text/plain;charset=UTF-8");
        connection->set_body("Internal server error");
        connection->set_status(websocketpp::http::status_code::internal_server_error);
        suppress_head_body(connection, method);
    }
}

void web_server_impl::serve_static_file(const server_t::connection_ptr& connection,
                                        std::string_view                method,
                                        boost::urls::segments_view      segments)
{
    using namespace websocketpp::http;

    constexpr std::string_view ALLOWED_METHODS = "GET, HEAD, OPTIONS";

    if (method == HTTP_OPTIONS) {
        connection->replace_header("Allow", std::string(ALLOWED_METHODS));
        connection->set_status(status_code::no_content);
        return;
    }

    if (method != HTTP_GET && method != HTTP_HEAD) {
        connection->replace_header("Allow", std::string(ALLOWED_METHODS));
        connection->replace_header("Cache-Control", "no-store");
        connection->replace_header("Content-Type", "text/plain;charset=UTF-8");
        connection->set_body("Method not allowed");
        connection->set_status(status_code::method_not_allowed);
        return;
    }

    const auto       path     = make_decoded_path(segments);
    std::string_view resource = path;
    if (resource == "/") {
        resource = "/index.html";
    }
    if (resource.starts_with('/')) {
        resource.remove_prefix(1);
    }

    const auto& files = static_files::get_web_files();
    const auto* file  = files.get_file(resource);

    if (file == nullptr) {
        connection->set_body(create_404_body(std::string(path)));
        connection->replace_header("Content-Type", "text/html;charset=UTF-8");
        connection->replace_header("Cache-Control", "no-store");
        connection->set_status(status_code::not_found);
        return;
    }

    const auto encoding = select_content_encoding(connection->get_request_header("Accept-Encoding"));
    connection->replace_header("Vary", "Accept-Encoding");

    if (encoding == content_encoding_e::not_acceptable) {
        connection->replace_header("Cache-Control", "no-store");
        connection->replace_header("Content-Type", "text/plain;charset=UTF-8");
        connection->set_body("No acceptable content encoding available");
        connection->set_status(status_code::not_acceptable);
        return;
    }

    const bool use_gzip = encoding == content_encoding_e::gzip;
    const auto etag     = std::format("\"{}\"", use_gzip ? file->gzip_hash : file->identity_hash);

    connection->replace_header("ETag", etag);
    connection->replace_header("Cache-Control", "no-cache");

    const auto if_none_match = connection->get_request_header("If-None-Match");
    if (!if_none_match.empty() && if_none_match_matches(if_none_match, etag)) {
        connection->set_status(status_code::not_modified);
        return;
    }

    const auto content_size = use_gzip ? file->gzipped.size() : file->size;
    if (use_gzip) {
        connection->replace_header("Content-Encoding", "gzip");
    }

    if (method == HTTP_HEAD) {
        connection->replace_header("Content-Length", std::to_string(content_size));
    } else if (use_gzip) {
        connection->set_body(std::string(reinterpret_cast<const char*>(file->gzipped.data()), content_size));
    } else {
        connection->set_body(file->unzip());
    }

    connection->replace_header("Content-Type", std::string(file->mime));
    connection->set_status(status_code::ok);
}

} // namespace miximus::web_server::detail
