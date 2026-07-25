#include "core/app_state.hpp"
#include "core/command_line_options.hpp"
#include "utils/filesystem.hpp"
#include "utils/string_utils.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace {
using namespace miximus;

TEST(CommandLineOptions, DefaultsSettingsPathRelativeToExecutable)
{
    char  executable[] = "/opt/miximus/bin/miximus";
    char* arguments[]  = {executable};

    const auto options = core::parse_command_line_options(1, arguments);

    EXPECT_EQ(options.settings_path, "/opt/miximus/bin/settings.json");
    EXPECT_EQ(options.log_level, spdlog::level::info);
    EXPECT_FALSE(options.stop_after.has_value());
    EXPECT_FALSE(options.render_thread_delay_test.has_value());
}

TEST(CommandLineOptions, ParsesRuntimeAndTestOptions)
{
    char  executable[]   = "miximus";
    char  log_trace[]    = "--log-trace";
    char  settings[]     = "--settings";
    char  settings_arg[] = "/tmp/test settings.json";
    char  stop_after[]   = "--stop-after";
    char  stop_arg[]     = "2.5";
    char  delay[]        = "--test-render-delay-ms";
    char  delay_arg[]    = "12";
    char  period[]       = "--test-render-delay-every";
    char  period_arg[]   = "120";
    char* arguments[]    = {
        executable,
        log_trace,
        settings,
        settings_arg,
        stop_after,
        stop_arg,
        delay,
        delay_arg,
        period,
        period_arg,
    };

    const auto options = core::parse_command_line_options(static_cast<int>(std::size(arguments)), arguments);

    EXPECT_EQ(options.log_level, spdlog::level::trace);
    EXPECT_EQ(options.settings_path, "/tmp/test settings.json");
    ASSERT_TRUE(options.stop_after.has_value());
    EXPECT_DOUBLE_EQ(options.stop_after->count(), 2.5);
    ASSERT_TRUE(options.render_thread_delay_test.has_value());
    EXPECT_EQ(options.render_thread_delay_test->delay, std::chrono::milliseconds{12});
    EXPECT_EQ(options.render_thread_delay_test->every_frames, 120);
}

TEST(CommandLineOptions, TreatsSettingsPathAsUtf8)
{
    char  executable[]   = "miximus";
    char  settings[]     = "--settings";
    char  settings_arg[] = "/tmp/r\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s.json";
    char* arguments[]    = {executable, settings, settings_arg};

    const auto options = core::parse_command_line_options(static_cast<int>(std::size(arguments)), arguments);

    EXPECT_EQ(utils::path_to_utf8(options.settings_path), settings_arg);
}

TEST(StringUtils, ConvertsWideStringsToUtf8)
{
    EXPECT_EQ(utils::wide_to_utf8(L"r\u00E4ksm\u00F6rg\u00E5s"), "r\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s");
}

TEST(StringUtils, ConvertsUtf8ToUtf32)
{
    EXPECT_EQ(utils::utf8_to_utf32("r\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s"), U"r\u00E4ksm\u00F6rg\u00E5s");
}

#ifdef _WIN32
TEST(CommandLineOptions, PreservesNativeWindowsSettingsPath)
{
    wchar_t  executable[]   = L"miximus";
    wchar_t  settings[]     = L"--settings";
    wchar_t  settings_arg[] = L"C:\\r\u00E4ksm\u00F6rg\u00E5s.json";
    wchar_t* arguments[]    = {executable, settings, settings_arg};

    const auto options = core::parse_command_line_options(static_cast<int>(std::size(arguments)), arguments);

    EXPECT_EQ(options.settings_path.native(), settings_arg);
}
#endif

TEST(CommandLineOptions, RequiresCompleteRenderDelayConfiguration)
{
    char  executable[] = "miximus";
    char  delay[]      = "--test-render-delay-ms";
    char  delay_arg[]  = "12";
    char* arguments[]  = {executable, delay, delay_arg};

    EXPECT_THROW((void)core::parse_command_line_options(static_cast<int>(std::size(arguments)), arguments),
                 std::invalid_argument);
}

TEST(CommandLineOptions, AreAvailableFromApplicationState)
{
    core::command_line_options_s options;
    options.render_thread_delay_test = core::render_thread_delay_test_options_s{
        .delay        = std::chrono::milliseconds{12},
        .every_frames = 120,
    };

    const core::app_state_s app(core::app_state_s::test_state_t{}, std::move(options));

    ASSERT_TRUE(app.command_line_options().render_thread_delay_test.has_value());
    EXPECT_EQ(app.command_line_options().render_thread_delay_test->delay, std::chrono::milliseconds{12});
    EXPECT_EQ(app.command_line_options().render_thread_delay_test->every_frames, 120);
}

} // namespace
