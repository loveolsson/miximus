#include "gpu/presenter.hpp"
#include "gpu/window.hpp"
#include "logger/logger.hpp"

#include <atomic>
#include <cstdlib>
#include <exception>
#include <string>

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <chrono>
#include <gtest/gtest.h>
#include <memory>
#include <thread>

namespace miximus::gpu { namespace {

using namespace std::chrono_literals;
class window_test : public testing::Test
{
  protected:
    static inline std::unique_ptr<window_system_s> system;
    static void                                    SetUpTestSuite()
    {
        logger::init_loggers(spdlog::level::warn);
        system = std::make_unique<window_system_s>();
    }

    static void    TearDownTestSuite() { system.reset(); }
    static vec2i_t on_screen_position()
    {
        int x{};
        int y{};
        int width{};
        int height{};
        glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), &x, &y, &width, &height);
        // Use visible desktop coordinates and an even grid on scaled XWayland displays.
        return {((x + 64) / 2) * 2, ((y + 64) / 2) * 2};
    }

    static void expect_geometry(window_s& window, recti_s expected)
    {
        const auto settle = std::chrono::steady_clock::now() + 250ms;
        while (std::chrono::steady_clock::now() < settle) {
            window_s::poll();
            std::this_thread::sleep_for(10ms);
        }

        const auto deadline = std::chrono::steady_clock::now() + 2s;
        while (true) {
            window_s::poll();
            if (window.get_window_rect() == expected && window.get_framebuffer_size() == expected.size) {
                break;
            }

            std::this_thread::sleep_for(10ms);
            if (std::chrono::steady_clock::now() >= deadline) {
                break;
            }
        }
        EXPECT_EQ(glfwGetWindowAttrib(window.native_window(), GLFW_CLIENT_API), GLFW_NO_API);
        EXPECT_EQ(window.get_window_rect(), expected);
        EXPECT_EQ(window.get_framebuffer_size(), expected.size);
    }
};

TEST_F(window_test, PresentsOnlyPublishedFramesAndRedrawsAfterResize)
{
    const recti_s initial_rect{
        .pos = on_screen_position(), .size = {320, 180}
    };

    window_s window({.rect = initial_rect});
    expect_geometry(window, initial_rect);
    // Test options are read from an environment that the test does not modify.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    device_s device({.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr, .presentation = true});
    auto     image = device.create_texture({.width = 32, .height = 32});

    auto commands = device.try_record();
    ASSERT_TRUE(commands);
    commands->clear(image, {1, 0, 0, 1});
    auto ready = commands->submit();
    commands.reset();
    {
        presenter_s presenter(device, window.native_window(), {.width = 320, .height = 180});
        const auto  wait_for_presents = [&](uint64_t count) {
            const auto deadline = std::chrono::steady_clock::now() + 3s;
            while (presenter.metrics().presents < count && presenter.metrics().failure.empty() &&
                   std::chrono::steady_clock::now() < deadline) {
                window_s::poll();
                std::this_thread::sleep_for(1ms);
            }

            EXPECT_TRUE(presenter.metrics().failure.empty());
            EXPECT_EQ(presenter.metrics().presents, count);
        };

        presenter.publish(image, ready);
        wait_for_presents(1);
        const auto settle = std::chrono::steady_clock::now() + 150ms;
        while (std::chrono::steady_clock::now() < settle) {
            window_s::poll();
            std::this_thread::sleep_for(1ms);
        }

        EXPECT_EQ(presenter.metrics().presents, 1);
        // An intentional repeat must still be submitted.
        presenter.publish(image, ready);
        wait_for_presents(2);
        glfwSetWindowSize(window.native_window(), 480, 270);
        expect_geometry(window,
                        recti_s{
                            .pos = on_screen_position(), .size = {480, 270}
        });
        presenter.resize({.width = 480, .height = 270});
        wait_for_presents(3);
        EXPECT_GE(presenter.metrics().recreations, 2);
    }

    EXPECT_EQ(device.validation_errors(), 0);
}

TEST_F(window_test, PresentsWhileOtherContextsSubmitAndRetire)
{
    window_s window({
        .rect = {.pos = on_screen_position(), .size = {320, 180}}
    });
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    device_s device({.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr, .presentation = true});
    auto     image    = device.create_texture({.width = 32, .height = 32});
    auto     commands = device.try_record();
    ASSERT_TRUE(commands);
    commands->clear(image, {1, 0, 0, 1});
    const auto ready = commands->submit();
    commands.reset();

    // Transfer traffic must remain independent of presentation, including while
    // the submission worker retires completed command pools and resources.
    auto                 background = device.create_recording_context(8);
    std::atomic_uint64_t submissions{};
    std::string          submission_failure;
    std::jthread         producer([&](const std::stop_token& stop) {
        try {
            while (!stop.stop_requested()) {
                if (auto recording = background.try_record()) {
                    auto target = device.create_texture({.width = 32, .height = 32});
                    recording->clear(target, {0, 1, 0, 1});
                    recording->submit();
                    ++submissions;
                }
                std::this_thread::sleep_for(1ms);
            }
        } catch (const std::exception& error) {
            submission_failure = error.what();
        }
    });

    std::atomic_uint64_t completions{};
    {
        presenter_s presenter(
            device,
            window.native_window(),
            {.width = 320, .height = 180},
            {.next_frame = [&](presentation_pacing_e, const std::stop_token&) -> std::optional<presentation_frame_s> {
                 return presentation_frame_s{.image = image, .ready = ready, .lease = {}};
             },
             .complete = [&](presentation_feedback_s) { ++completions; }});

        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (completions < 120 && presenter.metrics().failure.empty() &&
               std::chrono::steady_clock::now() < deadline) {
            window_s::poll();
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_GE(completions, 120);
        EXPECT_TRUE(presenter.metrics().failure.empty());
    }

    producer.request_stop();
    producer.join();
    EXPECT_TRUE(submission_failure.empty()) << submission_failure;
    EXPECT_GE(submissions, 120);
    EXPECT_EQ(device.validation_errors(), 0);
}

TEST_F(window_test, StopsWhileAcquiredImageWaitsForSourcePreroll)
{
    const recti_s initial_rect{
        .pos = on_screen_position(), .size = {320, 180}
    };

    window_s window({.rect = initial_rect});
    // Test options are read from an environment that the test does not modify.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    device_s device({.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr, .presentation = true});
    std::atomic_uint64_t requests{};
    {
        presenter_s presenter(
            device,
            window.native_window(),
            {.width = 320, .height = 180},
            {.next_frame = [&](presentation_pacing_e, const std::stop_token&) -> std::optional<presentation_frame_s> {
                 ++requests;
                 return std::nullopt;
             },
             .complete = {}});
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (requests == 0 && presenter.metrics().failure.empty() && std::chrono::steady_clock::now() < deadline) {
            window_s::poll();
            std::this_thread::sleep_for(1ms);
        }

        EXPECT_GT(requests, 0);
        EXPECT_EQ(presenter.metrics().presents, 0);
        EXPECT_TRUE(presenter.metrics().failure.empty());
        presenter.request_stop();
    }

    EXPECT_EQ(device.validation_errors(), 0);
}

TEST_F(window_test, WaitsForTheSelectedFrameWithoutRequestingAReplacement)
{
    window_s window({
        .rect = {.pos = on_screen_position(), .size = {320, 180}}
    });
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    device_s         device({.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr, .presentation = true});
    auto             image = device.create_texture({.width = 32, .height = 32});
    std::atomic_bool release_submission{};
    // Release even if a test assertion fails, so device teardown cannot deadlock.
    std::jthread release_guard([&](const std::stop_token& stop) {
        const auto deadline = std::chrono::steady_clock::now() + 3s;
        while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(1ms);
        }
        release_submission = true;
    });
    auto         blocker = device.try_record();
    ASSERT_TRUE(blocker);
    blocker->on_submitted([&](const completion_s&) {
        while (!release_submission) {
            std::this_thread::sleep_for(1ms);
        }
    });
    const auto blocked = blocker->submit();
    blocker.reset();
    auto commands = device.try_record();
    ASSERT_TRUE(commands);
    commands->clear(image, {1, 0, 0, 1});
    const auto ready = commands->submit();
    commands.reset();

    std::atomic_uint64_t requests{};
    std::atomic_uint64_t completions{};
    {
        presenter_s presenter(
            device,
            window.native_window(),
            {.width = 320, .height = 180},
            {.next_frame = [&](presentation_pacing_e, const std::stop_token&) -> std::optional<presentation_frame_s> {
                 ++requests;
                 if (completions != 0) {
                     return std::nullopt;
                 }
                 return presentation_frame_s{.image = image, .ready = ready, .lease = {}};
             },
             .complete =
                 [&](presentation_feedback_s) {
                     EXPECT_TRUE(ready.ready());
                     ++completions;
                 }});
        const auto selection_deadline = std::chrono::steady_clock::now() + 1s;
        while (requests == 0 && std::chrono::steady_clock::now() < selection_deadline) {
            window_s::poll();
            std::this_thread::sleep_for(1ms);
        }
        std::this_thread::sleep_for(30ms);
        EXPECT_EQ(requests, 1);
        EXPECT_FALSE(ready.ready());
        EXPECT_EQ(presenter.metrics().presents, 0);
        EXPECT_TRUE(presenter.metrics().failure.empty());

        release_submission             = true;
        const auto completion_deadline = std::chrono::steady_clock::now() + 2s;
        while (completions == 0 && std::chrono::steady_clock::now() < completion_deadline) {
            window_s::poll();
            std::this_thread::sleep_for(1ms);
        }
        EXPECT_EQ(completions, 1);
        EXPECT_EQ(presenter.metrics().presents, 1);
        EXPECT_TRUE(presenter.metrics().failure.empty());
    }
    EXPECT_EQ(device.validation_errors(), 0);
}

TEST_F(window_test, RestoresPixelSizeAndPositionOnCreationAndRecreation)
{
    ASSERT_EQ(glfwGetPlatform(), GLFW_PLATFORM_X11);
    const recti_s saved{
        .pos = on_screen_position(), .size = {320, 180}
    };

    {
        window_s window({.rect = saved});
        expect_geometry(window, saved);
    }

    {
        const recti_s changed{
            .pos = saved.pos + vec2i_t{120, 80 },
                .size = {480, 270}
        };

        window_s window({.rect = changed});
        expect_geometry(window, changed);
    }

    window_s restored({.rect = saved});
    expect_geometry(restored, saved);
}

TEST_F(window_test, FullscreenDoesNotReplaceSavedWindowGeometry)
{
    const auto monitors = window_s::get_monitors();
    ASSERT_FALSE(monitors.empty());
    const recti_s saved{
        .pos = on_screen_position(), .size = {400, 224}
    };

    for (const auto& monitor : monitors) {
        {
            window_s fullscreen({.fullscreen = true, .monitor_id = monitor.id, .rect = saved});
            EXPECT_NE(glfwGetWindowMonitor(fullscreen.native_window()), nullptr);
            window_s::poll();
        }

        window_s restored({.rect = saved});
        expect_geometry(restored, saved);
    }
}
}} // namespace miximus::gpu
