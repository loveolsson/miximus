#include "logger/logger.hpp"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

using namespace spdlog;

namespace miximus::logger {

void init_loggers(level::level_enum level_)
{
    init_thread_pool(8192, 1);

    auto sink = std::make_shared<sinks::stdout_color_sink_mt>();

    {
        auto log = std::make_shared<async_logger>("app", sink, thread_pool());
        log->flush_on(level_);
        log->set_level(level_);
        register_logger(log);

        log->info("Log level: {}", to_string_view(level_));
    }

    {
        auto log = std::make_shared<async_logger>("http", sink, thread_pool());
        log->flush_on(level_);
        log->set_level(level_);
        register_logger(log);
    }

    {
        auto log = std::make_shared<async_logger>("gpu", sink, thread_pool());
        log->flush_on(level_);
        log->set_level(level_);
        register_logger(log);
    }
}

} // namespace miximus::logger
