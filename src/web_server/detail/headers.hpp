#pragma once
#include <string>
#include <string_view>

namespace miximus::web_server::detail {

enum class content_encoding_e
{
    identity,
    gzip,
    not_acceptable,
};

content_encoding_e select_content_encoding(std::string_view header) noexcept;
bool               if_none_match_matches(std::string_view header, std::string_view etag) noexcept;
std::string        make_http_date();

} // namespace miximus::web_server::detail
