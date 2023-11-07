#include "web_server/server.hpp"
#include "web_server/detail/server_impl.hpp"

namespace miximus::web_server {

std::unique_ptr<server_s> create_web_server() { return std::make_unique<detail::web_server_impl>(); }

} // namespace miximus::web_server