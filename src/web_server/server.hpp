#pragma once
#include "types/topic.hpp"
#include "utils/asio.hpp"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <memory>

namespace miximus::web_server {

typedef std::function<void(nlohmann::json&&, int64_t)> callback_t;
typedef std::function<nlohmann::json()> config_getter_t;

class server_s
{
  public:
    server_s()          = default;
    virtual ~server_s() = default;

    virtual void subscribe(topic_e topic, const callback_t& callback)   = 0;
    virtual void set_config_getter(const config_getter_t& getter)       = 0;
    virtual void start(uint16_t port, boost::asio::io_service* service) = 0;
    virtual void stop()                                                 = 0;

    /**
     * Sync versions of calls should only be called from the callback thread
     */
    virtual void send_message(const nlohmann::json& msg, int64_t connection_id)      = 0;
    virtual void send_message_sync(const nlohmann::json& msg, int64_t connection_id) = 0;
    virtual void broadcast_message(const nlohmann::json& msg)                        = 0;
    virtual void broadcast_message_sync(const nlohmann::json& msg)                   = 0;
};

std::unique_ptr<server_s> create_web_server();

} // namespace miximus::web_server