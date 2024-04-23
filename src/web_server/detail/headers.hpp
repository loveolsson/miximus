#pragma once
#include <string_view>

namespace miximus::web_server::detail {

bool accept_encoding_has_gzip(std::string_view header) noexcept;

} // namespace miximus::web_server::detail