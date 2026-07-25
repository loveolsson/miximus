#include "command_line_options.hpp"

#include <boost/program_options.hpp>

#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>

namespace miximus::core {
namespace {

namespace program_options = boost::program_options;

program_options::options_description make_options_description()
{
    program_options::options_description description("Options");
    auto                                 add_option = description.add_options();
    add_option("help,h", "Show this help message");
    add_option("log-debug", "Enable debug logging");
    add_option("log-trace", "Enable trace logging");
    add_option("settings", program_options::value<std::string>(), "Path to the settings file");
    add_option("stop-after", program_options::value<double>(), "Stop after a positive number of seconds");
    add_option("test-render-delay-ms",
               program_options::value<uint64_t>(),
               "Test only: stall the render thread for this many milliseconds");
    add_option("test-render-delay-every",
               program_options::value<uint64_t>(),
               "Test only: inject the render-thread stall every N rendered frames");
    return description;
}

[[noreturn]] void throw_invalid_option(std::string message) { throw std::invalid_argument(std::move(message)); }

} // namespace

command_line_options_s parse_command_line_options(int argc, char* argv[])
{
    command_line_options_s result;
    result.settings_path = std::filesystem::path(argv[0]).parent_path() / "settings.json";

    const auto                     description = make_options_description();
    program_options::variables_map values;
    try {
        program_options::store(program_options::parse_command_line(argc, argv, description), values);
        program_options::notify(values);
    } catch (const program_options::error& error) {
        throw_invalid_option(error.what());
    }

    result.show_help = values.contains("help");

    if (values.contains("log-debug") && values.contains("log-trace")) {
        throw_invalid_option("--log-debug and --log-trace cannot be used together");
    }
    if (values.contains("log-debug")) {
        result.log_level = spdlog::level::debug;
    } else if (values.contains("log-trace")) {
        result.log_level = spdlog::level::trace;
    }

    if (values.contains("settings")) {
        result.settings_path = values["settings"].as<std::string>();
    }

    if (values.contains("stop-after")) {
        const auto seconds = values["stop-after"].as<double>();
        if (!std::isfinite(seconds) || seconds <= 0.0) {
            throw_invalid_option("--stop-after requires a positive number of seconds");
        }
        result.stop_after = std::chrono::duration<double>{seconds};
    }

    const bool has_render_delay  = values.contains("test-render-delay-ms");
    const bool has_render_period = values.contains("test-render-delay-every");
    if (has_render_delay != has_render_period) {
        throw_invalid_option("--test-render-delay-ms and --test-render-delay-every must be used together");
    }
    if (has_render_delay) {
        const auto delay_ms     = values["test-render-delay-ms"].as<uint64_t>();
        const auto every_frames = values["test-render-delay-every"].as<uint64_t>();
        if (delay_ms == 0 || delay_ms > static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
            throw_invalid_option("--test-render-delay-ms requires a positive integer");
        }
        if (every_frames == 0) {
            throw_invalid_option("--test-render-delay-every requires a positive integer");
        }
        result.render_thread_delay_test = render_thread_delay_test_options_s{
            .delay        = std::chrono::milliseconds{static_cast<int64_t>(delay_ms)},
            .every_frames = every_frames,
        };
    }

    return result;
}

std::string get_command_line_help(std::string_view executable_name)
{
    std::ostringstream output;
    output << "Usage: " << executable_name << " [options]\n" << make_options_description();
    return std::move(output).str();
}

} // namespace miximus::core
