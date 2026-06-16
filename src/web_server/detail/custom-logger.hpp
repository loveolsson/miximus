#pragma once
#include "logger/logger.hpp"

#include <string_view>
#include <websocketpp/common/cpp11.hpp>
#include <websocketpp/logger/basic.hpp>
#include <websocketpp/logger/levels.hpp>

namespace websocketpp::log {
template <typename concurrency, typename names>
class custom_logger : public basic<concurrency, names>
{
  public:
    typedef basic<concurrency, names> base;

    /**
     * @param hint A channel type specific hint for how to construct the logger
     */
    custom_logger(channel_type_hint::value hint = channel_type_hint::access)
        : basic<concurrency, names>(hint)
        , m_channel_type_hint(hint)
    {
    }

    /**
     * @param channels A set of channels to statically enable
     * @param hint A channel type specific hint for how to construct the logger
     */
    custom_logger(level channels, channel_type_hint::value hint = channel_type_hint::access)
        : basic<concurrency, names>(channels, hint)
        , m_channel_type_hint(hint)
    {
    }

    /**
     * @param channel The channel to write to
     * @param msg The message to write
     */
    void write(level channel, std::string const& msg) { write(channel, msg.c_str()); }

    /**
     * @param channel The channel to write to
     * @param msg The message to write
     */
    void write(level channel, char const* msg)
    {
        if (!this->dynamic_test(channel)) {
            return;
        }

        auto log = getlog("http");
        if (!log) {
            return;
        }

        if (m_channel_type_hint == channel_type_hint::access) {
            log->info(msg);
        } else {
            switch (channel) {
                case elevel::devel:
                case elevel::library:
                    log->debug(msg);
                    break;
                case elevel::info: {
                    // "Error getting remote endpoint: system:9" and
                    // "asio async_shutdown error: system:9" are emitted by
                    // websocketpp when it tears down the pre-allocated accept
                    // socket after stop_listening() cancels the pending
                    // async_accept.  That socket was never assigned a valid fd
                    // by the OS, so EBADF is unavoidable.  These are not
                    // indicative of a real problem; route them to debug.
                    std::string_view sv(msg);
                    if (sv.starts_with("asio async_shutdown error: system:9") ||
                        sv.starts_with("Error getting remote endpoint: system:9")) {
                        log->debug(msg);
                    } else {
                        log->info(msg);
                    }
                    break;
                }
                case elevel::warn:
                    log->warn(msg);
                    break;
                case elevel::rerror:
                    log->error(msg);
                    break;
                case elevel::fatal:
                    log->critical(msg);
                    break;

                default:
                    break;
            }
        }
    }

  private:
    channel_type_hint::value m_channel_type_hint;
};

} // namespace websocketpp::log