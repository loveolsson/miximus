#include "core/app_state.hpp"
#include "core/command_line_options.hpp"
#include "utils/filesystem.hpp"
#include "utils/string_utils.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace miximus;

template <typename Character, std::size_t Size>
auto make_arguments(std::array<std::basic_string<Character>, Size>& values)
{
    std::array<Character*, Size> result{};
    std::ranges::transform(values, result.begin(), [](auto& value) { return value.data(); });
    return result;
}

TEST(CommandLineOptions, DefaultsSettingsPathRelativeToExecutable)
{
    auto argument_values = std::array{std::string{"/opt/miximus/bin/miximus"}};
    auto arguments       = make_arguments(argument_values);

    const auto options = core::parse_command_line_options(static_cast<int>(arguments.size()), arguments.data());

    EXPECT_EQ(options.settings_path, "/opt/miximus/bin/settings.json");
    EXPECT_EQ(options.log_level, spdlog::level::info);
    EXPECT_FALSE(options.stop_after.has_value());
    EXPECT_FALSE(options.render_thread_delay_test.has_value());
}

TEST(CommandLineOptions, ParsesRuntimeAndTestOptions)
{
    auto argument_values = std::array{
        std::string{"miximus"},
        std::string{"--log-trace"},
        std::string{"--settings"},
        std::string{"/tmp/test settings.json"},
        std::string{"--stop-after"},
        std::string{"2.5"},
        std::string{"--test-render-delay-ms"},
        std::string{"12"},
        std::string{"--test-render-delay-every"},
        std::string{"120"},
    };
    auto arguments = make_arguments(argument_values);

    const auto options = core::parse_command_line_options(static_cast<int>(arguments.size()), arguments.data());

    EXPECT_EQ(options.log_level, spdlog::level::trace);
    EXPECT_EQ(options.settings_path, "/tmp/test settings.json");
    ASSERT_TRUE(options.stop_after.has_value());
    EXPECT_DOUBLE_EQ(options.stop_after.value_or(std::chrono::duration<double>{}).count(), 2.5);
    ASSERT_TRUE(options.render_thread_delay_test.has_value());
    const auto delay_test = options.render_thread_delay_test.value_or(core::render_thread_delay_test_options_s{});
    EXPECT_EQ(delay_test.delay, std::chrono::milliseconds{12});
    EXPECT_EQ(delay_test.every_frames, 120);
}

TEST(CommandLineOptions, TreatsSettingsPathAsUtf8)
{
    auto argument_values = std::array{
        std::string{"miximus"},
        std::string{"--settings"},
        std::string{"/tmp/r\xC3\xA4ksm\xC3\xB6rg\xC3\xA5s.json"},
    };
    auto arguments = make_arguments(argument_values);

    const auto options = core::parse_command_line_options(static_cast<int>(arguments.size()), arguments.data());

    EXPECT_EQ(utils::path_to_utf8(options.settings_path), argument_values[2]);
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
    auto argument_values = std::array{
        std::wstring{L"miximus"},
        std::wstring{L"--settings"},
        std::wstring{L"C:\\r\u00E4ksm\u00F6rg\u00E5s.json"},
    };
    auto arguments = make_arguments(argument_values);

    const auto options = core::parse_command_line_options(static_cast<int>(arguments.size()), arguments.data());

    EXPECT_EQ(options.settings_path.native(), argument_values[2]);
}
#endif

TEST(CommandLineOptions, RequiresCompleteRenderDelayConfiguration)
{
    auto argument_values = std::array{std::string{"miximus"}, std::string{"--test-render-delay-ms"}, std::string{"12"}};
    auto arguments       = make_arguments(argument_values);

    EXPECT_THROW((void)core::parse_command_line_options(static_cast<int>(arguments.size()), arguments.data()),
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
    const auto delay_test =
        app.command_line_options().render_thread_delay_test.value_or(core::render_thread_delay_test_options_s{});
    EXPECT_EQ(delay_test.delay, std::chrono::milliseconds{12});
    EXPECT_EQ(delay_test.every_frames, 120);
}

} // namespace
