#pragma once
#include <spdlog/spdlog.h>

std::shared_ptr<spdlog::logger> getlog(const std::string& name);

namespace miximus::logger {

void init_loggers(spdlog::level::level_enum level);

} // namespace miximus::logger
