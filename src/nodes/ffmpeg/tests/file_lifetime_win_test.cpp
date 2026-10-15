#include "nodes/ffmpeg/detail/decoder.hpp"
#include "utils/filesystem.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

namespace miximus::nodes::ffmpeg::detail { namespace {

TEST(DecoderFileLifetime, WindowsSeesOpenHandleWhileIdleAndAcrossLoopSeeks)
{
    const auto path = std::filesystem::temp_directory_path() /
                      ("miximus-handle-" + std::to_string(utils::flicks_now().count()) + ".y4m");
    {
        std::ofstream file(path, std::ios::binary);
        file << "YUV4MPEG2 W4 H2 F25:1 Ip A1:1 C420jpeg\nFRAME\n" << std::string(12, 'x');
    }
    std::atomic_bool stopped{};
    {
        decoder_s decoder(utils::path_to_utf8(path), false, 4, stopped);
        for (int loop = 0; loop < 3; ++loop) {
            // FFmpeg's Windows read handle does not share deletion. This checks
            // the native lifetime, even when all compressed data fits in AVIO.
            std::error_code error;
            EXPECT_FALSE(std::filesystem::remove(path, error));
            EXPECT_TRUE(error);
            EXPECT_NE(decoder.next(), nullptr);
            EXPECT_EQ(decoder.next(), nullptr);
            decoder.rewind();
        }
    }
    std::error_code error;
    EXPECT_TRUE(std::filesystem::remove(path, error));
    EXPECT_FALSE(error);
}

}} // namespace miximus::nodes::ffmpeg::detail
