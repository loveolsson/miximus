#pragma once
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace miximus::web_server::detail {

struct html_escape_s
{
    std::string_view value;
};

constexpr html_escape_s html_escape(std::string_view value) noexcept { return {value}; }

inline std::ostream& operator<<(std::ostream& output, html_escape_s escaped)
{
    for (const char character : escaped.value) {
        switch (character) {
            case '<':
                output << "&lt;";
                break;
            case '>':
                output << "&gt;";
                break;
            case '&':
                output << "&amp;";
                break;
            case '"':
                output << "&quot;";
                break;
            default:
                output << character;
                break;
        }
    }
    return output;
}

inline std::string create_404_body(std::string_view resource)
{
    std::ostringstream output;
    output << "<!doctype html><html><head>"
           << "<title>Error 404 (Resource not found)</title>"
           << "</head><body>"
           << "<h1>Error 404</h1>"
           << "<p>The requested URL " << html_escape(resource) << " was not found on this server.</p>"
           << "</body></html>";
    return std::move(output).str();
}

} // namespace miximus::web_server::detail
