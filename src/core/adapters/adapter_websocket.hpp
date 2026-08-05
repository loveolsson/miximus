#pragma once
#include "core/configuration_fwd.hpp"
#include "core/node_manager.hpp"
#include "render/font/font_registry_fwd.hpp"
#include "web_server/server_fwd.hpp"

#include <memory>

namespace miximus::core {

std::unique_ptr<node_manager_s::adapter_i> create_websocket_adapter(node_manager_s&          manager,
                                                                    configuration_s&         configuration,
                                                                    web_server::server_s&    server,
                                                                    render::font_registry_s& font_registry);

} // namespace miximus::core
