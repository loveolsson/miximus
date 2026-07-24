#pragma once
#include <format>
#include <string>
#include <string_view>

namespace miximus::web_server::detail {

inline std::string html_escape(std::string_view value)
{
    std::string result;
    result.reserve(value.size());
    for (const char character : value) {
        switch (character) {
            case '<':
                result += "&lt;";
                break;
            case '>':
                result += "&gt;";
                break;
            case '&':
                result += "&amp;";
                break;
            case '"':
                result += "&quot;";
                break;
            default:
                result += character;
                break;
        }
    }
    return result;
}

inline std::string create_404_body(std::string_view resource)
{
    return std::format("<!doctype html><html><head>"
                       "<title>Error 404 (Resource not found)</title><body>"
                       "<h1>Error 404</h1>"
                       "<p>The requested URL {} was not found on this server.</p>"
                       "</body></head></html>",
                       html_escape(resource));
}

} // namespace miximus::web_server::detail
