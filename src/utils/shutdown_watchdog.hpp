#pragma once

#include <string>

namespace miximus::utils {

void start_shutdown_watchdog();
void begin_shutdown_step(std::string step_info);
void report_shutdown_step_completed();
void finish_shutdown_watchdog();

} // namespace miximus::utils
