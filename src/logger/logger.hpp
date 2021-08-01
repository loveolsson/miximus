#pragma once
#include <spdlog/spdlog.h>

static auto& getlog = spdlog::get;

namespace miximus::logger {

void init_loggers(spdlog::level::level_enum level);

} // namespace miximus::logger
