#include "utils/failure_shutdown.hpp"
#include "utils/process_id.hpp"
#include "utils/shutdown_watchdog.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>

namespace miximus::utils { namespace {
using namespace std::chrono_literals;

class recovery_files_test : public testing::Test
{
  public:
    std::filesystem::path directory;
    void                  SetUp() override
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        directory = std::filesystem::temp_directory_path() / std::format("miximus-recovery-{}-{}", process_id(), stamp);
        ASSERT_TRUE(std::filesystem::create_directory(directory));
    }
    void               TearDown() override { std::filesystem::remove_all(directory); }
    static std::string read(const std::filesystem::path& path)
    {
        std::ifstream file(path);
        return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    }
};

TEST_F(recovery_files_test, RecoveryNeverOverwritesOriginalOrPreviousRecovery)
{
    const auto original = directory / "settings.json";
    std::ofstream(original) << "original";
    const recovery_settings_s settings{.path = original, .contents = "{\"nodes\": []}"};
    const auto                first  = write_recovery_settings(settings);
    const auto                second = write_recovery_settings(settings);
    EXPECT_NE(first, second);
    EXPECT_EQ(read(original), "original");
    EXPECT_EQ(read(first), settings.contents);
    EXPECT_EQ(read(second), settings.contents);
}

TEST_F(recovery_files_test, NormalSaveReplacesCompleteContents)
{
    const auto original = directory / "settings.json";
    std::ofstream(original) << "previous longer settings contents";
    write_settings_atomically({.path = original, .contents = "{}"});
    EXPECT_EQ(read(original), "{}");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
}

TEST_F(recovery_files_test, FailedReplacementPreservesExistingDestination)
{
    const auto destination = directory / "settings.json";
    ASSERT_TRUE(std::filesystem::create_directory(destination));
    std::ofstream(destination / "keep") << "original";
    EXPECT_THROW(write_settings_atomically({.path = destination, .contents = "{}"}), std::exception);
    EXPECT_EQ(read(destination / "keep"), "original");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- GoogleTest death-test macro expansion.
TEST_F(recovery_files_test, FatalThreadPreservesStackAndSavesBeforeWatchdogExit)
{
    const auto original = directory / "settings.json";
    std::ofstream(original) << "original";
    EXPECT_EXIT(
        {
            initialize_shutdown_monitor();
            start_shutdown_watchdog(1s);
            publish_recovery_settings(original, "{\"recovered\": true}");
            struct retained_s
            {
                ~retained_s() { std::abort(); }
            } retained;
            fail_without_unwinding("test resource ownership failure");
        },
        testing::ExitedWithCode(EXIT_FAILURE),
        "Recovery settings saved to");
    EXPECT_EQ(read(original), "original");
    size_t recovered = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path() != original) {
            EXPECT_EQ(read(entry.path()), "{\"recovered\": true}");
            ++recovered;
        }
    }
    EXPECT_EQ(recovered, 1);
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- GoogleTest death-test macro expansion.
TEST_F(recovery_files_test, NormalSaveIsRejectedEvenAfterFailureTeardownCompletes)
{
    const auto original = directory / "settings.json";
    std::ofstream(original) << "original";
    EXPECT_EXIT(
        {
            initialize_shutdown_monitor();
            start_shutdown_watchdog(1s);
            publish_recovery_settings(original, "{}");
            request_failure_shutdown("unrecoverable error with successful teardown");
            finish_shutdown_watchdog();
            try {
                write_settings_atomically({.path = original, .contents = "unsafe replacement"});
                std::_Exit(EXIT_FAILURE);
            } catch (const std::runtime_error&) {
                std::_Exit(EXIT_SUCCESS);
            }
        },
        testing::ExitedWithCode(EXIT_SUCCESS),
        "Recovery settings saved to");
    EXPECT_EQ(read(original), "original");
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 2);
}

TEST_F(recovery_files_test, ReportedFailureCanFinishGracefullyWithoutWatchdogExit)
{
    const auto original = directory / "settings.json";
    EXPECT_EXIT(
        {
            initialize_shutdown_monitor();
            start_shutdown_watchdog(1s);
            publish_recovery_settings(original, "{}");
            request_failure_shutdown("recoverable worker shutdown");
            finish_shutdown_watchdog();
            std::_Exit(EXIT_SUCCESS); // Test child has completed the graceful failure handoff.
        },
        testing::ExitedWithCode(EXIT_SUCCESS),
        "Recovery settings saved to");
    EXPECT_FALSE(std::filesystem::exists(original));
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity) -- GoogleTest death-test macro expansion.
TEST_F(recovery_files_test, UnavailableSettingsDirectoryFallsBackToTemporaryDirectory)
{
    const auto filename = directory.filename().string() + ".json";
    const auto original = directory / "missing" / filename;
    EXPECT_EXIT(
        {
            initialize_shutdown_monitor();
            start_shutdown_watchdog(1s);
            publish_recovery_settings(original, "{}");
            request_failure_shutdown("test unavailable settings directory");
            finish_shutdown_watchdog();
            std::_Exit(EXIT_SUCCESS);
        },
        testing::ExitedWithCode(EXIT_SUCCESS),
        "Recovery settings saved to");
    size_t recovered = 0;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::temp_directory_path())) {
        const auto name = entry.path().filename().string();
        if (name.starts_with(filename + ".recovery-") && entry.path().extension() == ".json") {
            EXPECT_EQ(read(entry.path()), "{}");
            std::filesystem::remove(entry.path());
            ++recovered;
        }
    }
    EXPECT_EQ(recovered, 1);
    EXPECT_FALSE(std::filesystem::exists(original));
}

TEST_F(recovery_files_test, UnexpectedTerminateUsesRecoveryAndWatchdog)
{
    const auto original = directory / "settings.json";
    EXPECT_EXIT(
        {
            initialize_shutdown_monitor();
            start_shutdown_watchdog(1s);
            publish_recovery_settings(original, "{}");
            std::terminate();
        },
        testing::ExitedWithCode(EXIT_FAILURE),
        "Unhandled exception or noexcept violation");
    EXPECT_FALSE(std::filesystem::exists(original));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
}

TEST_F(recovery_files_test, WatchdogAlsoSavesWhenMainThreadNeverReportsFailure)
{
    const auto original = directory / "settings.json";
    EXPECT_EXIT(
        {
            initialize_shutdown_monitor();
            publish_recovery_settings(original, "{}");
            start_shutdown_watchdog(1s);
            std::this_thread::sleep_for(10s);
        },
        testing::ExitedWithCode(EXIT_FAILURE),
        "Recovery settings saved to");
    EXPECT_FALSE(std::filesystem::exists(original));
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(directory), std::filesystem::directory_iterator()), 1);
}

}} // namespace miximus::utils
