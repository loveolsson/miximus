#include "nodes/ffmpeg/detail/decoder.hpp"
#include "utils/filesystem.hpp"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <vector>

namespace miximus::nodes::ffmpeg::detail { namespace {

class DecoderTest : public testing::Test
{
    std::filesystem::path path;

  protected:
    void SetUp() override
    {
        path = std::filesystem::temp_directory_path() /
               utils::path_from_utf8("miximus-video-\xC3\xA5-" + std::to_string(utils::flicks_now().count()) + ".y4m");
        std::ofstream file(path, std::ios::binary);
        file << "YUV4MPEG2 W4 H2 F25:1 Ip A1:1 C420jpeg\n";
        const std::array<unsigned char, 12> pixels{16, 235, 16, 235, 16, 235, 16, 235, 128, 128, 128, 128};
        for (int i = 0; i < 3; ++i) {
            file << "FRAME\n";
            file.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
        }
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove(path, error);
    }

    std::string filename() const { return utils::path_to_utf8(path); }
};

TEST_F(DecoderTest, DrainsAndRewindsWithoutLosingLastFrame)
{
    std::atomic_bool stopped{};
    decoder_s        decoder(filename(), false, 4, stopped);
    for (int iteration = 0; iteration < 2; ++iteration) {
        for (int i = 0; i < 3; ++i) {
            auto frame = decoder.next();
            ASSERT_NE(frame, nullptr);
            EXPECT_EQ(frame->pts, utils::to_flicks(i / 25.0));
            EXPECT_EQ(frame->duration, utils::to_flicks(1.0 / 25.0));
        }

        EXPECT_EQ(decoder.next(), nullptr);
        decoder.rewind();
    }
}

TEST_F(DecoderTest, KeepsYuvSamplesForSharedGpuConversion)
{
    std::atomic_bool stopped{};
    decoder_s        decoder(filename(), false, 4, stopped);
    auto             frame = decoder.next();
    ASSERT_NE(frame, nullptr);
    frame->prepare_host();
    EXPECT_EQ(frame->dimensions, (gpu::vec2i_t{4, 2}));
    std::vector<std::byte> bytes(frame->buffer_bytes);
    frame->copy_to(bytes);
    gpu::video_buffer_header_s header;
    std::memcpy(&header, bytes.data(), sizeof(header));
    EXPECT_EQ(header.component_count, 3);
    EXPECT_EQ(header.components[0].stride, 4);
    EXPECT_EQ(bytes[header.components[0].offset], std::byte{16});
    EXPECT_EQ(bytes[header.components[0].offset + 1], std::byte{235});
    EXPECT_EQ(bytes[header.components[1].offset], std::byte{128});
    EXPECT_EQ(bytes[header.components[2].offset], std::byte{128});
}

TEST(DecodedFrame, RejectsHardwareDownloadBeforeAccessingSurface)
{
    decoded_frame_s decoded;
    auto*           frame = decoded.native_frame();
    frame->format         = AV_PIX_FMT_D3D11;
    // No native device: rejecting this frame must not invoke a transfer callback.
    frame->hw_frames_ctx = av_buffer_alloc(1);
    ASSERT_NE(frame->hw_frames_ctx, nullptr);
    EXPECT_TRUE(decoded.hardware_frame());
    EXPECT_THROW(decoded.prepare_host(), std::runtime_error);
    EXPECT_EQ(frame->format, AV_PIX_FMT_D3D11);
}

TEST(DecodedFrame, SemiplanarChromaCopiesOnlyItsSubsampledRows)
{
    for (const auto format : {AV_PIX_FMT_NV12, AV_PIX_FMT_P010LE}) {
        decoded_frame_s decoded;
        auto*           frame = decoded.native_frame();
        frame->format         = format;
        frame->width          = 4;
        frame->height         = 2;
        ASSERT_GE(av_frame_get_buffer(frame, 0), 0);
        const int row_bytes = format == AV_PIX_FMT_NV12 ? 4 : 8;
        for (int y = 0; y < 2; ++y) {
            std::memset(frame->data[0] + (static_cast<ptrdiff_t>(y) * frame->linesize[0]), 16, row_bytes);
        }

        std::memset(frame->data[1], 128, row_bytes);
        decoded.prepare_host();
        // Three color components, but only two planes: one Y and one UV.
        EXPECT_EQ(decoded.buffer_bytes, sizeof(gpu::video_buffer_header_s) + (size_t{3} * row_bytes));
        std::vector<std::byte> bytes(decoded.buffer_bytes);
        decoded.copy_to(bytes);
        EXPECT_EQ(bytes.back(), std::byte{128});
    }
}

TEST(DecodedFrame, CarriesEncodedAlphaModeWithoutCpuPremultiplication)
{
    for (const auto mode : {AVALPHA_MODE_UNSPECIFIED, AVALPHA_MODE_STRAIGHT, AVALPHA_MODE_PREMULTIPLIED}) {
        decoded_frame_s decoded;
        auto*           frame = decoded.native_frame();
        frame->format         = AV_PIX_FMT_RGBA;
        frame->width          = 2;
        frame->height         = 1;
        frame->alpha_mode     = mode;
        ASSERT_GE(av_frame_get_buffer(frame, 0), 0);
        const std::array<unsigned char, 8> pixels{80, 40, 20, 128, 255, 128, 64, 0};
        std::memcpy(frame->data[0], pixels.data(), pixels.size());
        decoded.prepare_host();
        const auto& header = decoded.component_header();
        EXPECT_EQ(header.component_count, 4);
        EXPECT_EQ(header.encoded_premultiplied_alpha, mode == AVALPHA_MODE_PREMULTIPLIED);
        std::vector<std::byte> bytes(decoded.buffer_bytes);
        decoded.copy_to(bytes);
        EXPECT_EQ(std::memcmp(bytes.data() + header.components[0].offset, pixels.data(), pixels.size()), 0);
        EXPECT_EQ(header.components[3].offset, header.components[0].offset + 3);
        EXPECT_EQ(header.components[3].encoding, gpu::video_component_encoding(8, 0, 0, 0, false));
    }
}

}} // namespace miximus::nodes::ffmpeg::detail
