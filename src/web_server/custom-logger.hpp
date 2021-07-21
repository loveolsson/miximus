#pragma once
#include "logger/logger.hpp"

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
    custom_logger<concurrency, names>(channel_type_hint::value hint = channel_type_hint::access)
        : basic<concurrency, names>(hint)
        , m_channel_type_hint(hint)
    {
    }

    /**
     * @param channels A set of channels to statically enable
     * @param hint A channel type specific hint for how to construct the logger
     */
    custom_logger<concurrency, names>(level channels, channel_type_hint::value hint = channel_type_hint::access)
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

        auto log = spdlog::get("http");
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
                case elevel::info:
                    log->info(msg);
                    break;
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
    typedef typename base::scoped_lock_type scoped_lock_type;
    channel_type_hint::value                m_channel_type_hint;
};

} // namespace websocketpp::log