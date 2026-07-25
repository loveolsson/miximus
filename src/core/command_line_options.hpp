#pragma once
#include <spdlog/common.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace miximus::core {

struct render_thread_delay_test_options_s
{
    std::chrono::milliseconds delay{};
    uint64_t                  every_frames{};
};

struct command_line_options_s
{
    spdlog::level::level_enum                         log_level{spdlog::level::info};
    std::filesystem::path                             settings_path;
    std::optional<std::chrono::duration<double>>      stop_after;
    std::optional<render_thread_delay_test_options_s> render_thread_delay_test;
    bool                                              show_help{};
};

command_line_options_s parse_command_line_options(int argc, char* argv[]);
std::string            get_command_line_help(std::string_view executable_name);

} // namespace miximus::core
