#include "url_parser.hpp"
#include <algorithm>
#include <cctype>

namespace miximus::web_server::detail {

url_parser url_parser::parse(std::string_view resource) { return url_parser(resource); }

url_parser::url_parser(std::string_view resource)
{
    // Find fragment (#) first, since it comes after everything else
    auto             fragment_pos     = resource.find('#');
    std::string_view working_resource = resource;

    if (fragment_pos != std::string_view::npos) {
        fragment_        = resource.substr(fragment_pos + 1);
        working_resource = resource.substr(0, fragment_pos);
    }

    // Find query string (?)
    auto query_pos = working_resource.find('?');
    if (query_pos != std::string_view::npos) {
        path_             = working_resource.substr(0, query_pos);
        auto query_string = working_resource.substr(query_pos + 1);
        parse_query_string(query_string);
    } else {
        path_ = working_resource;
    }

    // Ensure path is not empty - default to "/"
    if (path_.empty()) {
        static const std::string default_path = "/";
        path_                                 = std::string_view(default_path);
    }
}

std::string_view url_parser::get_query_param(std::string_view name) const
{
    auto it = query_params_.find(name);
    if (it != query_params_.end()) {
        return it->second;
    }
    return {};
}

bool url_parser::has_query_param(std::string_view name) const
{
    return query_params_.find(name) != query_params_.end();
}

void url_parser::parse_query_string(std::string_view query)
{
    if (query.empty()) {
        return;
    }

    size_t start = 0;
    while (start < query.length()) {
        // Find next parameter separator
        auto end = query.find('&', start);
        if (end == std::string_view::npos) {
            end = query.length();
        }

        auto param = query.substr(start, end - start);

        // Split parameter into name and value
        auto equals_pos = param.find('=');
        if (equals_pos != std::string_view::npos) {
            auto name  = param.substr(0, equals_pos);
            auto value = param.substr(equals_pos + 1);

            // Store raw values - user can call url_decode() if needed
            query_params_[name] = value;
        } else {
            // Parameter without value (flag parameter)
            auto name = param;

            // For flag parameters, we store an empty string_view
            static const std::string empty_value;
            query_params_[name] = std::string_view(empty_value);
        }

        start = end + 1;
    }
}

std::string url_parser::url_decode(std::string_view encoded)
{
    std::string result;
    result.reserve(encoded.length());

    for (size_t i = 0; i < encoded.length(); ++i) {
        char c = encoded[i];

        if (c == '%' && i + 2 < encoded.length()) {
            // Parse hex digits
            char hex1 = encoded[i + 1];
            char hex2 = encoded[i + 2];

            if (std::isxdigit(hex1) && std::isxdigit(hex2)) {
                // Convert hex to character
                int value = 0;
                if (hex1 >= '0' && hex1 <= '9')
                    value += (hex1 - '0') * 16;
                else if (hex1 >= 'A' && hex1 <= 'F')
                    value += (hex1 - 'A' + 10) * 16;
                else if (hex1 >= 'a' && hex1 <= 'f')
                    value += (hex1 - 'a' + 10) * 16;

                if (hex2 >= '0' && hex2 <= '9')
                    value += (hex2 - '0');
                else if (hex2 >= 'A' && hex2 <= 'F')
                    value += (hex2 - 'A' + 10);
                else if (hex2 >= 'a' && hex2 <= 'f')
                    value += (hex2 - 'a' + 10);

                result.push_back(static_cast<char>(value));
                i += 2; // Skip the two hex digits
            } else {
                result.push_back(c);
            }
        } else if (c == '+') {
            // '+' is often used to represent space in query strings
            result.push_back(' ');
        } else {
            result.push_back(c);
        }
    }

    return result;
}

} // namespace miximus::web_server::detail
