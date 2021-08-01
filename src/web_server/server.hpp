#pragma once
#include "types/topic.hpp"
#include "utils/asio.hpp"

#include <nlohmann/json_fwd.hpp>

#include <functional>
#include <memory>
#include <vector>

namespace miximus::web_server {
namespace detail {
class web_server_impl;
}

typedef std::function<void(nlohmann::json&&, int64_t)> callback_t;

class server_s
{
    std::unique_ptr<detail::web_server_impl> impl;

  public:
    server_s();
    ~server_s();

    void subscribe(topic_e topic, const callback_t& callback);

    void start(uint16_t port, boost::asio::io_service* service);
    void stop();

    /**
     * Sync versions of calls should only be called from the callback thread
     */
    void send_message(const nlohmann::json& msg, int64_t connection_id);
    void send_message_sync(const nlohmann::json& msg, int64_t connection_id);
    void broadcast_message(const nlohmann::json& msg);
    void broadcast_message_sync(const nlohmann::json& msg);
};
} // namespace miximus::web_server