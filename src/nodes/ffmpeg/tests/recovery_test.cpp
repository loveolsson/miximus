#include "gpu/device.hpp"
#include "logger/logger.hpp"
#include "nodes/ffmpeg/detail/playback.hpp"
#include "nodes/ffmpeg/registry.hpp"
#include "utils/filesystem.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <thread>

namespace miximus::nodes::ffmpeg { namespace {
using namespace std::chrono_literals;

class PlaybackRecovery : public testing::Test
{
    std::filesystem::path                                    path_;
    std::unique_ptr<gpu::device_s>                           device_;
    std::unique_ptr<gpu::transfer::texture_upload_service_s> uploads_;
    std::unique_ptr<ffmpeg_registry_s>                       registry_;
    std::shared_ptr<detail::playback_session_s>              session_;

  protected:
    void SetUp() override
    {
        if (!spdlog::get("gpu")) {
            logger::init_loggers(spdlog::level::info);
        }
        path_     = std::filesystem::temp_directory_path() /
                    ("miximus-recovery-" + std::to_string(utils::flicks_now().count()) + ".y4m");
        device_   = std::make_unique<gpu::device_s>(gpu::device_options_s{.validation = true});
        uploads_  = std::make_unique<gpu::transfer::texture_upload_service_s>(*device_);
        registry_ = std::make_unique<ffmpeg_registry_s>(*device_, *uploads_);
    }

    void TearDown() override
    {
        registry_->retire(std::move(session_));
        registry_.reset();
        uploads_.reset();
        EXPECT_EQ(device_->validation_errors(), 0U);
        device_.reset();
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    void write_source(bool valid)
    {
        std::ofstream file(path_, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(file);
        if (valid) {
            file << "YUV4MPEG2 W640 H360 F25:1 Ip A1:1 C420jpeg\n";
            const std::string pixels(640 * 360 * 3 / 2, static_cast<char>(128));
            for (int i = 0; i < 120; ++i) {
                file << "FRAME\n" << pixels;
            }
        } else {
            file << "not yet synchronized";
        }
        file.close();
        ASSERT_TRUE(file);
    }

    void start()
    {
        session_ = registry_->create({.filename = utils::path_to_utf8(path_), .attempt_hardware_decode = false});
    }

    detail::playback_session_s& session() { return *session_; }

    bool wait_ready(bool ready)
    {
        const auto deadline = std::chrono::steady_clock::now() + 15s;
        while (std::chrono::steady_clock::now() < deadline) {
            const auto status = session_->status();
            if (ready ? status.ready && status.error.empty() : !status.ready && !status.error.empty()) {
                return true;
            }
            std::this_thread::sleep_for(5ms);
        }
        return false;
    }

    void retire()
    {
        std::weak_ptr<detail::playback_session_s> weak = session_;
        registry_->retire(std::move(session_));
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (!weak.expired() && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        EXPECT_TRUE(weak.expired());
    }
};

TEST_F(PlaybackRecovery, MissingFileAppearsWithoutChangingOptions)
{
    start();
    ASSERT_TRUE(wait_ready(false));
    EXPECT_NE(session().status().error.find("Open video file"), std::string::npos);
    write_source(true);
    ASSERT_TRUE(wait_ready(true)) << session().status().error;
    session().advance({});
    EXPECT_NE(session().selected(), nullptr);
}

TEST_F(PlaybackRecovery, IncompleteFileRecoversAndRewindDuringRetryIsHonored)
{
    write_source(false);
    start();
    ASSERT_TRUE(wait_ready(false));
    session().advance(utils::to_flicks(1000.0));
    session().rewind();
    write_source(true);
    ASSERT_TRUE(wait_ready(true)) << session().status().error;
    session().advance({});
    const auto frame = session().selected();
    ASSERT_NE(frame, nullptr);
    EXPECT_EQ(frame->pts, utils::flicks{});
    EXPECT_LT(session().status().decoded_frames, 20U);
}

TEST_F(PlaybackRecovery, InterruptedSourceRetainsLastFrameAndResumesWithoutBacklog)
{
    write_source(true);
    start();
    ASSERT_TRUE(wait_ready(true));
    session().advance({});
    ASSERT_NE(session().selected(), nullptr);
    write_source(false);
    auto       position = utils::flicks{};
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (session().status().error.empty() && std::chrono::steady_clock::now() < deadline) {
        position += utils::to_flicks(0.04);
        session().advance(position);
        std::this_thread::sleep_for(5ms);
    }
    ASSERT_TRUE(wait_ready(false)) << session().status().error << " decoded=" << session().status().decoded_frames
                                   << " queue=" << session().status().queued_frames;
    const auto held = session().selected();
    ASSERT_NE(held, nullptr);
    EXPECT_TRUE(session().status().fallback_reason.empty());
    write_source(true);
    ASSERT_TRUE(wait_ready(true)) << session().status().error;
    EXPECT_EQ(session().selected(), held);
    const auto before = session().status().decoded_frames;
    session().advance(position + utils::to_flicks(0.08));
    const auto resumed = session().selected();
    ASSERT_NE(resumed, nullptr);
    EXPECT_NE(resumed, held);
    EXPECT_GE(resumed->pts, position - utils::to_flicks(0.04));
    EXPECT_LT(session().status().decoded_frames - before, 20U);
}

TEST_F(PlaybackRecovery, RetiringUnavailableSourceInterruptsBackoff)
{
    start();
    ASSERT_TRUE(wait_ready(false));
    retire();
}

}} // namespace miximus::nodes::ffmpeg
