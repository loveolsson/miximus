#pragma once
#include <string>

struct GLFWmonitor;

namespace miximus::gpu::detail {

std::string get_monitor_id(GLFWmonitor* monitor);

} // namespace miximus::gpu::detail
