#include "logger/logger.hpp"

#include <spdlog/async.h>
#include <spdlog/async_logger.h>
#include <spdlog/sinks/stdout_color_sinks.h>

using namespace spdlog;

namespace miximus::logger {

void init_loggers(level::level_enum level_)
{
    auto sink = std::make_shared<sinks::stdout_color_sink_mt>();

    auto create_logger = [&](auto name) {
        auto log = std::make_shared<spdlog::logger>(name, sink);
        log->flush_on(level_);
        log->set_level(level_);
        register_logger(log);
        return log;
    };

    auto app_logger = create_logger("app");
    create_logger("http");
    create_logger("gpu");
    create_logger("nodes");
    create_logger("decklink");

    app_logger->info("Log level: {}", to_string_view(level_));
}

} // namespace miximus::logger
