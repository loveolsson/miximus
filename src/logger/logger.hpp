#pragma once
#include <spdlog/spdlog.h>

#include <string>
#include <string_view>
#include <utility>

static auto& getlog = spdlog::get;

namespace miximus::logger {

void init_loggers(spdlog::level::level_enum level);

template <typename... Args>
void log_error_noexcept(std::string_view logger_name, spdlog::format_string_t<Args...> format, Args&&... args) noexcept
{
    try {
        if (const auto log = spdlog::get(std::string(logger_name)); log != nullptr) {
            log->error(format, std::forward<Args>(args)...);
        }
    } catch (...) { // NOLINT(bugprone-empty-catch) -- intended for foreign callback and error paths
    }
}

} // namespace miximus::logger
