#include "web_server/server.hpp"

#include "web_server/detail/server_impl.hpp"

#include <memory>

namespace miximus::web_server {

std::shared_ptr<server_s> create_web_server() { return std::make_shared<detail::web_server_impl>(); }

} // namespace miximus::web_server
