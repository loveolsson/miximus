#include "core/app_state.hpp"
#include "gpu/device.hpp"
#include "logger/logger.hpp"
#include "nodes/ffmpeg/detail/playback.hpp"
#include "nodes/ffmpeg/registry.hpp"
#include "utils/environment.hpp"

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace miximus::nodes::ffmpeg { namespace {

class PlaybackWithoutCef : public testing::Test
{
  protected:
    static inline std::unique_ptr<core::app_state_s> application;

    static void SetUpTestSuite()
    {
        if (!spdlog::get("gpu")) {
            logger::init_loggers(spdlog::level::info);
        }
        application = std::make_unique<core::app_state_s>();
    }

    static void TearDownTestSuite() { application.reset(); }
};

TEST_F(PlaybackWithoutCef, SoftwareFallbackReleasesBudgetForAnotherSession)
{
    const auto filename = utils::environment_value("MIXIMUS_FFMPEG_UHD_TEST_VIDEO");
    if (!filename) {
        GTEST_SKIP() << "Set MIXIMUS_FFMPEG_UHD_TEST_VIDEO to a hardware-decodable UHD video fixture";
    }
    auto& app = *application;
    auto  first =
        app.ffmpeg_registry()->create({.filename = *filename, .attempt_hardware_decode = true, .buffered_frames = 8});
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while ((first->status().backend != "software" || first->status().queued_frames != 8) &&
           first->status().error.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto fallback = first->status();
    ASSERT_EQ(fallback.backend, "software") << fallback.error;
    ASSERT_EQ(fallback.queued_frames, 8U) << fallback.error;
    EXPECT_NE(fallback.fallback_reason.find("budget exhausted"), std::string::npos);

    // Leave the first session paused with a full software queue. Its old UHD
    // textures must not consume the budget needed by this hardware session.
    auto second =
        app.ffmpeg_registry()->create({.filename = *filename, .attempt_hardware_decode = true, .buffered_frames = 1});
    deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!second->status().ready && second->status().error.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto recovered = second->status();
    EXPECT_TRUE(recovered.ready) << recovered.error;
    EXPECT_EQ(recovered.backend, "d3d11va") << recovered.fallback_reason;
    EXPECT_EQ(recovered.transfer_path, "D3D11 shared GPU texture to Vulkan");
    EXPECT_TRUE(recovered.error.empty()) << recovered.error;
    EXPECT_EQ(app.gpu()->validation_errors(), 0U);
    app.ffmpeg_registry()->retire(std::move(first));
    app.ffmpeg_registry()->retire(std::move(second));
}

TEST_F(PlaybackWithoutCef, ImportsHardwareFramesUsingApplicationDevice)
{
    const auto filename = utils::environment_value("MIXIMUS_FFMPEG_TEST_VIDEO");
    if (!filename) {
        GTEST_SKIP() << "Set MIXIMUS_FFMPEG_TEST_VIDEO to a hardware-decodable video fixture";
    }
    auto& app = *application;
    ASSERT_EQ(app.gpu()->external_image_import_support(), gpu::external_image_import_support_e::supported);

    auto       session  = app.ffmpeg_registry()->create({.filename = *filename, .attempt_hardware_decode = true});
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!session->status().ready && session->status().error.empty() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const auto status = session->status();
    EXPECT_TRUE(status.ready) << status.error;
    EXPECT_TRUE(status.error.empty()) << status.error;
    EXPECT_EQ(status.backend, "d3d11va") << status.fallback_reason;
    EXPECT_EQ(status.transfer_path, "D3D11 shared GPU texture to Vulkan");
    session->advance({});
    const auto frame = session->selected();
    ASSERT_NE(frame, nullptr);
    EXPECT_NE(frame->gpu_texture, nullptr);
    EXPECT_EQ(app.gpu()->validation_errors(), 0U);
    app.ffmpeg_registry()->retire(std::move(session));
}

}} // namespace miximus::nodes::ffmpeg
