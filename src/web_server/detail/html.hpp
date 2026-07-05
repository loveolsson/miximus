#pragma once
#include <format>
#include <string>

namespace miximus::web_server::detail {

inline std::string html_escape(const std::string& s)
{
    std::string r;
    r.reserve(s.size());
    for (const char c : s) {
        switch (c) {
            case '<':
                r += "&lt;";
                break;
            case '>':
                r += "&gt;";
                break;
            case '&':
                r += "&amp;";
                break;
            case '"':
                r += "&quot;";
                break;
            default:
                r += c;
                break;
        }
    }
    return r;
}

inline std::string create_404_body(const std::string& resource)
{
    return std::format("<!doctype html><html><head>"
                       "<title>Error 404 (Resource not found)</title><body>"
                       "<h1>Error 404</h1>"
                       "<p>The requested URL {} was not found on this server.</p>"
                       "</body></head></html>",
                       html_escape(resource));
}

} // namespace miximus::web_server::detail
