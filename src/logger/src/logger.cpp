#include "logger/logger.hpp"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace miximus::logger {

void init_loggers()
{
    spdlog::init_thread_pool(8192, 1);

    auto sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();

    {
        auto log = std::make_shared<spdlog::async_logger>("web-server", sink, spdlog::thread_pool());
        log->flush_on(spdlog::level::info);
        log->set_level(spdlog::level::level_enum::info);
        spdlog::register_logger(log);
    }

    {
        auto log = std::make_shared<spdlog::async_logger>("application", sink, spdlog::thread_pool());
        log->flush_on(spdlog::level::info);
        log->set_level(spdlog::level::level_enum::info);
        spdlog::register_logger(log);
    }

    {
        auto log = std::make_shared<spdlog::async_logger>("gpu", sink, spdlog::thread_pool());
        log->flush_on(spdlog::level::info);
        log->set_level(spdlog::level::level_enum::info);
        spdlog::register_logger(log);
    }
}

} // namespace miximus::logger
