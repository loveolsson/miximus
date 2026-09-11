#include "gpu/device.hpp"
#include "gpu/drawing.hpp"
#include "logger/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <gtest/gtest.h>
#include <latch>
#include <semaphore>
#include <stdexcept>
#include <thread>

namespace miximus::gpu { namespace {

class device_test : public testing::Test
{
  protected:
    static inline std::unique_ptr<device_s> device;
    static void                             SetUpTestSuite()
    {
        logger::init_loggers(spdlog::level::warn);
        device_options_s options;
        // Test options are read from an environment that the test does not modify.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
        // Test options are read from an environment that the test does not modify.
        // NOLINTNEXTLINE(concurrency-mt-unsafe)
        if (const auto uuid = std::getenv("MIXIMUS_VULKAN_DEVICE")) {
            options.device_uuid = uuid;
        }

        options.max_recordings       = 3;
        options.descriptor_page_size = 4;
        device                       = std::make_unique<device_s>(options);
    }

    static void TearDownTestSuite()
    {
        if (!device) {
            return;
        }

        device->collect();
        EXPECT_EQ(device->validation_errors(), 0U);
        device.reset();
    }

    static void finish(std::unique_ptr<recording_s>& record)
    {
        ASSERT_NE(record, nullptr);
        const auto token = record->submit();
        ASSERT_EQ(token.wait(std::chrono::seconds(5)), wait_result_e::ready);
    }
};

TEST_F(device_test, OddPaddedRoundTripsPreserveEveryActiveByteAndPadding)
{
    for (auto format : {format_e::rgba_unorm8, format_e::rgba_unorm16, format_e::rgba16_float, format_e::r32_uint}) {
        const size_t bpp    = format == format_e::rgba_unorm8 || format == format_e::r32_uint ? 4 : 8;
        const size_t stride = 256;
        const size_t bytes  = stride * 5;
        auto         input  = device->create_buffer(bytes, host_access_e::sequential_write, 64);
        auto         output = device->create_buffer(bytes, host_access_e::readback, 64);
        auto         image  = device->create_texture({.width = 7, .height = 5}, format);
        auto         memory = input.writable_bytes();
        for (size_t i = 0; i < bytes; ++i) {
            memory[i] = std::byte((i * 13 + 17) % 256);
        }

        std::vector<std::byte> expected(memory.begin(), memory.end());

        auto record = device->try_record();
        record->copy(input, output, bytes); // Define padding on the GPU too.
        record->upload(input, image, stride);
        record->readback(image, output, stride);
        EXPECT_THROW(output.readable_bytes(), std::logic_error);
        finish(record);

        const auto result = output.readable_bytes();
        EXPECT_TRUE(std::ranges::equal(result, expected)) << "bytes per pixel: " << bpp;
    }
}

TEST_F(device_test, MipmapsFilterTheWholeImageAndRegenerateOnlyFromSubmittedWrites)
{
    auto source =
        device->create_texture({.width = 8, .height = 8}, format_e::rgba_unorm8, sampling_e::mipmapped_linear);
    auto target = device->create_texture({.width = 1, .height = 1}, format_e::rgba_unorm8);
    auto input  = device->create_buffer(size_t{8} * 8 * 4, host_access_e::sequential_write);
    auto output = device->create_buffer(4, host_access_e::readback);
    EXPECT_EQ(source.mip_levels(), 4U);
    auto bytes = input.writable_bytes();
    std::ranges::fill(bytes, std::byte{0});
    for (size_t y = 0; y < 8; ++y) {
        for (size_t x = 0; x < 8; ++x) {
            bytes[(y * 8 + x) * 4]       = std::byte{x >= 3 && x <= 4 && y >= 3 && y <= 4 ? uint8_t{255} : uint8_t{0}};
            bytes[((y * 8 + x) * 4) + 3] = std::byte{255};
        }
    }

    auto record = device->try_record();
    record->upload(input, source);
    record->generate_mip_maps(source);
    finish(record);

    // Producer-generated levels survive the handoff to a separate recording.
    record = device->try_record();
    record->draw(source, target, {.compositing = compositing_e::replace});
    record->readback(target, output);
    finish(record);
    EXPECT_NEAR(std::to_integer<int>(output.readable_bytes()[0]), 16, 1);
    {
        auto aborted = device->try_record();
        aborted->clear(source, {0, 0, 1, 1});
        aborted->generate_mip_maps(source);
        aborted->draw(source, target, {.compositing = compositing_e::replace});
    }

    record = device->try_record();
    record->draw(source, target, {.compositing = compositing_e::replace});
    record->readback(target, output);
    finish(record);
    EXPECT_NEAR(std::to_integer<int>(output.readable_bytes()[0]), 16, 1);
    record = device->try_record();
    record->clear(source, {0, 1, 0, 1});
    record->draw(source, target, {.compositing = compositing_e::replace});
    record->readback(target, output);
    finish(record);

    const std::array<std::byte, 4> green{std::byte{0}, std::byte{255}, std::byte{0}, std::byte{255}};
    EXPECT_TRUE(std::ranges::equal(output.readable_bytes(), green));
}

TEST_F(device_test, OddAndOneDimensionalMipChainsClampToOneTexel)
{
    EXPECT_THROW(device->create_texture({8, 8}, format_e::r32_uint, sampling_e::mipmapped_linear),
                 std::invalid_argument);
    for (const auto extent : {
             extent_s{.width = 7, .height = 5},
             extent_s{.width = 1, .height = 9},
             extent_s{.width = 9, .height = 1},
             extent_s{.width = 1, .height = 1}
    }) {
        auto source = device->create_texture(extent, format_e::rgba_unorm16, sampling_e::mipmapped_linear);
        auto target = device->create_texture({.width = 1, .height = 1}, format_e::rgba_unorm8);
        auto output = device->create_buffer(4, host_access_e::readback);

        auto record = device->try_record();
        record->clear(source, {1, 0, 0, 1});
        record->draw(source, target, {.compositing = compositing_e::replace});
        record->readback(target, output);
        finish(record);

        const std::array<std::byte, 4> red{std::byte{255}, std::byte{0}, std::byte{0}, std::byte{255}};
        EXPECT_TRUE(std::ranges::equal(output.readable_bytes(), red));
    }
}

TEST_F(device_test, ClippingPreservesPixelsOutsideTheRequestedViewport)
{
    auto source = device->create_texture({.width = 1, .height = 1}, format_e::rgba_unorm8);
    auto target = device->create_texture({.width = 4, .height = 3}, format_e::rgba_unorm8);
    auto output = device->create_buffer(size_t{4} * 3 * 4, host_access_e::readback);

    auto record = device->try_record();
    record->clear(source, {1, 0, 0, 1});
    record->clear(target, {0, 1, 0, 1});
    draw_s draw{.compositing = compositing_e::replace};
    draw.clip = {1, 1, 2, 1};
    record->draw(source, target, draw);
    draw.clip = {-1, 0, 2, 1};
    record->draw(source, target, draw);
    draw.clip = {10, 10, 2, 2};
    record->draw(source, target, draw);
    record->readback(target, output);
    finish(record);

    const auto bytes = output.readable_bytes();
    for (size_t y = 0; y < 3; ++y) {
        for (size_t x = 0; x < 4; ++x) {
            const bool red = (y == 0 && x == 0) || (y == 1 && (x == 1 || x == 2));
            EXPECT_EQ(bytes[(y * 4 + x) * 4], red ? std::byte{255} : std::byte{0});
            EXPECT_EQ(bytes[(((y * 4) + x) * 4) + 1], red ? std::byte{0} : std::byte{255});
        }
    }
}

TEST_F(device_test, MirroredMixGeometryPreservesNegativeDestinationScales)
{
    auto                         input = device->create_buffer(8, host_access_e::sequential_write);
    const std::array<uint8_t, 8> pixels{255, 0, 0, 255, 0, 0, 255, 255};
    std::memcpy(input.writable_bytes().data(), pixels.data(), pixels.size());
    auto source = device->create_texture({.width = 2, .height = 1}, format_e::rgba_unorm8, sampling_e::nearest);
    auto target = device->create_texture({.width = 2, .height = 1}, format_e::rgba_unorm8);
    auto output = device->create_buffer(8, host_access_e::readback);

    auto record = device->try_record();
    record->upload(input, source);
    const mix_s parameters{
        .a_destination = {1, 0, -1, 1},
        .fraction      = 0,
        .compositing   = compositing_e::replace,
    };

    record->mix(source, source, target, parameters);
    record->readback(target, output);
    finish(record);

    std::array<uint8_t, 8> actual{};
    std::memcpy(actual.data(), output.readable_bytes().data(), actual.size());
    const std::array<uint8_t, 8> expected{0, 0, 255, 255, 255, 0, 0, 255};
    EXPECT_EQ(actual, expected);
}

TEST_F(device_test, AbortedRecordingReleasesHostAndDoesNotCommitImageLayout)
{
    auto input  = device->create_buffer(64, host_access_e::sequential_write);
    auto output = device->create_buffer(64, host_access_e::readback);
    auto image  = device->create_texture({.width = 4, .height = 4}, format_e::rgba_unorm8);
    std::ranges::fill(input.writable_bytes(), std::byte{123});
    {
        auto aborted = device->try_record();
        aborted->upload(input, image);
        aborted->readback(image, output);
        EXPECT_THROW(input.writable_bytes(), std::logic_error);
        EXPECT_NE(device->try_record(), nullptr);
    }

    EXPECT_NO_THROW(input.writable_bytes());

    auto record = device->try_record();
    record->clear(image, {0, 1, 0, 1});
    record->readback(image, output);
    finish(record);

    const auto result = output.readable_bytes();
    for (size_t i = 0; i < result.size(); i += 4) {
        EXPECT_EQ(result[i], std::byte{0});
        EXPECT_EQ(result[i + 1], std::byte{255});
        EXPECT_EQ(result[i + 2], std::byte{0});
        EXPECT_EQ(result[i + 3], std::byte{255});
    }
}

TEST_F(device_test, OrderedDrawsPreservePremultipliedBlendingAndOrientation)
{
    auto                          input = device->create_buffer(16, host_access_e::sequential_write);
    const std::array<uint8_t, 16> pixels{128, 0, 0, 128, 0, 128, 0, 128, 0, 0, 128, 128, 128, 128, 128, 128};
    std::memcpy(input.writable_bytes().data(), pixels.data(), pixels.size());
    auto source = device->create_texture({.width = 2, .height = 2}, format_e::rgba_unorm8);
    auto target = device->create_texture({.width = 2, .height = 2}, format_e::rgba_unorm16);
    auto output = device->create_buffer(32, host_access_e::readback);

    auto record = device->try_record();
    record->upload(input, source);
    record->clear(target, {0, 0, 0, 1});
    record->draw(source, target);
    record->draw(source, target);
    record->readback(target, output);
    finish(record);

    std::array<uint16_t, 16> result{};
    std::memcpy(result.data(), output.readable_bytes().data(), sizeof(result));
    const double alpha    = 128.0 / 255.0;
    const double expected = alpha * (2 - alpha) * 65535;
    for (size_t i = 0; i < pixels.size(); ++i) {
        const double component = pixels.at(i) != 0 ? expected : 0;
        EXPECT_NEAR(result.at(i), i % 4 == 3 ? 65535 : component, 2);
    }
}

TEST_F(device_test, SubmittedUsesSurviveResourceReleaseAndReuseAcrossFrames)
{
    auto         output = device->create_buffer(64, host_access_e::readback);
    completion_s token;
    {
        auto source = device->create_texture({.width = 4, .height = 4}, format_e::rgba_unorm8);
        auto first  = device->try_record();
        first->clear(source, {1, 0, 0, 1});
        token       = first->submit();
        auto second = device->try_record();
        ASSERT_NE(second, nullptr);
        second->clear(source, {0, 0, 1, 1});
        second->readback(source, output);
        token = second->submit();
    }

    EXPECT_EQ(token.wait(std::chrono::seconds(5)), wait_result_e::ready);

    const auto bytes = output.readable_bytes();
    EXPECT_EQ(bytes[0], std::byte{0});
    EXPECT_EQ(bytes[2], std::byte{255});
    device->collect();
}

TEST_F(device_test, RejectsInvalidSizesAndStridesBeforeRecordingCommands)
{
    EXPECT_THROW(device->create_texture({0, 5}), std::invalid_argument);
    EXPECT_THROW(device->create_buffer(32, host_access_e::readback, 3), std::invalid_argument);
    auto input  = device->create_buffer(256, host_access_e::sequential_write);
    auto target = device->create_texture({.width = 7, .height = 5}, format_e::rgba_unorm8);

    auto record = device->try_record();
    EXPECT_THROW(record->upload(input, target, 25), std::invalid_argument);
    EXPECT_THROW(record->upload(input, target, 24), std::invalid_argument);
    EXPECT_THROW(record->upload(input, target, 256), std::invalid_argument);
}

TEST_F(device_test, BufferConversionPreservesOddRowsAndPadding)
{
    constexpr size_t stride = 256;
    constexpr size_t bytes  = stride * 3;
    auto             input  = device->create_buffer(bytes, host_access_e::sequential_write);
    auto             output = device->create_buffer(bytes, host_access_e::readback);
    auto             image  = device->create_texture({.width = 17, .height = 3}, format_e::rgba_unorm16);
    auto             memory = input.writable_bytes();
    for (size_t i = 0; i < bytes; ++i) {
        memory[i] = std::byte((i * 13 + 97) % 256);
    }

    const std::vector<std::byte> expected(memory.begin(), memory.end());

    auto record = device->try_record();
    record->copy(input, output, bytes);
    record->unpack_rgba(input, image, stride);
    record->pack_rgba(image, output, stride);
    finish(record);
    EXPECT_TRUE(std::ranges::equal(output.readable_bytes(), expected));
}

TEST_F(device_test, ChannelOrdersDecodeToCanonicalRGBAAndEncodeBack)
{
    constexpr std::array<std::array<uint8_t, 4>, 4> stored{
        {{17, 43, 91, 255}, {91, 43, 17, 255}, {91, 43, 17, 9}, {255, 17, 43, 91}}
    };

    for (uint32_t order = 0; order < stored.size(); ++order) {
        auto input     = device->create_buffer(4, host_access_e::sequential_write);
        auto canonical = device->create_buffer(8, host_access_e::readback);
        auto output    = device->create_buffer(4, host_access_e::readback);
        auto image     = device->create_texture({.width = 1, .height = 1}, format_e::rgba_unorm16);
        std::memcpy(input.writable_bytes().data(), stored.at(order).data(), 4);

        auto record = device->try_record();
        record->unpack_rgba(input, image, 4, static_cast<channel_order_e>(order));
        record->readback(image, canonical);
        record->pack_rgba(image, output, 4, static_cast<channel_order_e>(order));
        finish(record);

        std::array<uint16_t, 4> actual{};
        std::memcpy(actual.data(), canonical.readable_bytes().data(), sizeof(actual));
        for (size_t c = 0; c < 4; ++c) {
            EXPECT_EQ(actual.at(c), stored.at(0).at(c) * 257);
        }

        auto expected = stored.at(order);
        if (order == 2) {
            expected[3] = 255;
        }

        EXPECT_EQ(std::memcmp(output.readable_bytes().data(), expected.data(), 4), 0);
    }
}

TEST_F(device_test, V210LegalBlackAndWhiteDecodeAndPackWithDeterministicPadding)
{
    constexpr size_t width   = 7;
    constexpr size_t height  = 2;
    constexpr size_t stride  = 128;
    auto             raw     = device->create_buffer(stride * height, host_access_e::sequential_write);
    auto             packed  = device->create_buffer(stride * height, host_access_e::readback);
    auto             linear  = device->create_buffer(width * height * 8, host_access_e::readback);
    auto             working = device->create_texture({.width = width, .height = height}, format_e::rgba_unorm16);
    std::array<uint32_t, stride * height / 4> words{};
    for (size_t y = 0; y < height; ++y) {
        const uint32_t luma = (y != 0U) ? 940 : 64;
        for (size_t group = 0; group < 2; ++group) {
            const size_t start  = (y * stride / 4) + (group * 4);
            words.at(start)     = 512 | (luma << 10) | (512 << 20);
            words.at(start + 1) = luma | (512 << 10) | (luma << 20);
            words.at(start + 2) = 512 | (luma << 10) | (512 << 20);
            words.at(start + 3) = luma | (512 << 10) | (luma << 20);
        }
    }

    std::memcpy(raw.writable_bytes().data(), words.data(), sizeof(words));
    for (const auto weights : {
             std::array{.299f,  .114f },
             std::array{.2126f, .0722f},
             std::array{.2627f, .0593f}
    }) {
        const float     kr = weights[0];
        const float     kb = weights[1];
        const float     kg = 1 - kr - kb;
        constexpr float ys = 1023.F / 876.F;
        constexpr float cs = 1023.F / 896.F;

        // Rows include an explicit fourth padding element, matching the shader layout.
        color_transform_s decode;
        decode.matrix = {ys,
                         0,
                         2 * (1 - kr) * cs,
                         0,
                         ys,
                         -2 * kb * (1 - kb) / kg * cs,
                         -2 * kr * (1 - kr) / kg * cs,
                         0,
                         ys,
                         2 * (1 - kb) * cs,
                         0,
                         0};
        decode.offset = {64.F / 1023.F, 512.F / 1023.F, 512.F / 1023.F, 0};

        color_transform_s encode;
        encode.matrix = {kr / ys,
                         kg / ys,
                         kb / ys,
                         0,
                         -kr / (2 * (1 - kb) * cs),
                         -kg / (2 * (1 - kb) * cs),
                         .5f / cs,
                         0,
                         .5f / cs,
                         -kg / (2 * (1 - kr) * cs),
                         -kb / (2 * (1 - kr) * cs),
                         0};
        encode.offset = decode.offset;

        auto record = device->try_record();
        record->unpack_v210(raw, working, decode, stride);
        record->readback(working, linear);
        record->pack_v210(working, packed, encode, stride);
        finish(record);

        std::array<uint16_t, width * height * 4> rgb{};
        std::memcpy(rgb.data(), linear.readable_bytes().data(), sizeof(rgb));
        for (size_t i = 0; i < rgb.size(); ++i) {
            EXPECT_NEAR(rgb.at(i), i >= width * 4 || i % 4 == 3 ? 65535 : 0, 2);
        }

        std::array<uint32_t, words.size()> actual{};
        std::memcpy(actual.data(), packed.readable_bytes().data(), sizeof(actual));
        EXPECT_EQ(actual, words);
    }
}

TEST_F(device_test, V210PackingMatchesIndependentRec709PrimaryColorReference)
{
    constexpr uint32_t width   = 7;
    auto               upload  = device->create_buffer(size_t{width} * 8, host_access_e::sequential_write);
    auto               output  = device->create_buffer(128, host_access_e::readback);
    auto               working = device->create_texture({.width = width, .height = 1}, format_e::rgba_unorm16);
    const std::array<std::array<uint16_t, 4>, width> pixels{
        {{65535, 0, 0, 65535},
         {0, 65535, 0, 65535},
         {0, 0, 65535, 65535},
         {16384, 16384, 16384, 65535},
         {4096, 32768, 8192, 65535},
         {65535, 65535, 65535, 65535},
         {0, 0, 0, 65535}}
    };

    std::memcpy(upload.writable_bytes().data(), pixels.data(), sizeof(pixels));
    constexpr float   kr = .2126f;
    constexpr float   kb = .0722f;
    constexpr float   kg = 1 - kr - kb;
    constexpr float   ys = 876.F / 1023.F;
    constexpr float   cs = 896.F / 1023.F;
    color_transform_s encode;
    encode.matrix = {kr * ys,
                     kg * ys,
                     kb * ys,
                     0,
                     -kr / (2 * (1 - kb)) * cs,
                     -kg / (2 * (1 - kb)) * cs,
                     .5f * cs,
                     0,
                     .5f * cs,
                     -kg / (2 * (1 - kr)) * cs,
                     -kb / (2 * (1 - kr)) * cs,
                     0};
    encode.offset = {64.F / 1023.F, 512.F / 1023.F, 512.F / 1023.F, 0};

    auto record = device->try_record();
    record->upload(upload, working);
    record->pack_v210(working, output, encode, 128);
    finish(record);

    std::array<uint32_t, 32> actual{};
    std::memcpy(actual.data(), output.readable_bytes().data(), sizeof(actual));
    auto yuv = [&](size_t x) {
        std::array<double, 3> rgb{};
        for (size_t c = 0; c < 3; ++c) {
            double v  = pixels.at(std::min(x, size_t{width - 1})).at(c) / 65535.0;
            rgb.at(c) = v < .018 ? v * 4.5 : (1.099 * std::pow(v, .45)) - .099;
        }

        const double y = (.2126 * rgb[0]) + (.7152 * rgb[1]) + (.0722 * rgb[2]);
        return std::array{
            64 + (876 * y), 512 + (448 * (rgb[2] - y) / (1 - .0722)), 512 + (448 * (rgb[0] - y) / (1 - .2126))};
    };

    for (size_t group = 0; group < 2; ++group) {
        std::array<std::array<double, 3>, 6> values{};
        for (size_t x = 0; x < 6; ++x) {
            values.at(x) = yuv((group * 6) + x);
        }

        const std::array<double, 12> expected{(values[0][1] + values[1][1]) / 2,
                                              values[0][0],
                                              (values[0][2] + values[1][2]) / 2,
                                              values[1][0],
                                              (values[2][1] + values[3][1]) / 2,
                                              values[2][0],
                                              (values[2][2] + values[3][2]) / 2,
                                              values[3][0],
                                              (values[4][1] + values[5][1]) / 2,
                                              values[4][0],
                                              (values[4][2] + values[5][2]) / 2,
                                              values[5][0]};
        for (size_t c = 0; c < expected.size(); ++c) {
            EXPECT_NEAR((actual.at((group * 4) + (c / 3)) >> ((c % 3) * 10)) & 1023, std::round(expected.at(c)), 1);
        }
    }

    for (size_t i = 8; i < actual.size(); ++i) {
        EXPECT_EQ(actual.at(i), 0U);
    }
}

TEST_F(device_test, EmptyHandlesAndRepeatedSubmissionAreRejected)
{
    buffer_s  buffer;
    texture_s image;
    EXPECT_THROW(buffer.readable_bytes(), std::logic_error);

    auto record = device->try_record();
    EXPECT_THROW(record->upload(buffer, image), std::invalid_argument);
    finish(record);
    EXPECT_THROW(record->submit(), std::logic_error);
}

TEST_F(device_test, LinearAndVideoMixUseIndependentPerDrawParameters)
{
    auto black       = device->create_texture({.width = 2, .height = 1}, format_e::rgba_unorm16);
    auto white       = device->create_texture({.width = 2, .height = 1}, format_e::rgba_unorm16);
    auto linear      = device->create_texture({.width = 2, .height = 1}, format_e::rgba_unorm16);
    auto video       = device->create_texture({.width = 2, .height = 1}, format_e::rgba_unorm16);
    auto linear_host = device->create_buffer(16, host_access_e::readback);
    auto video_host  = device->create_buffer(16, host_access_e::readback);

    auto record = device->try_record();
    record->clear(black, {0, 0, 0, 1});
    record->clear(white, {1, 1, 1, 1});
    mix_textures(*record, &black, &white, &linear, .5, {}, {}, blend_mode_e::linear);
    mix_textures(*record, &black, &white, &video, .5, {}, {}, blend_mode_e::video);
    record->readback(linear, linear_host);
    record->readback(video, video_host);
    finish(record);

    std::array<uint16_t, 8> linear_pixels{};
    std::array<uint16_t, 8> video_pixels{};
    std::memcpy(linear_pixels.data(), linear_host.readable_bytes().data(), 16);
    std::memcpy(video_pixels.data(), video_host.readable_bytes().data(), 16);
    const double expected_video = std::pow((.5 + .099) / 1.099, 1 / .45) * 65535;
    for (size_t i = 0; i < 8; ++i) {
        EXPECT_NEAR(linear_pixels.at(i), i % 4 == 3 ? 65535 : 32767.5, 1);
        EXPECT_NEAR(video_pixels.at(i), i % 4 == 3 ? 65535 : expected_video, 2);
    }
}

TEST_F(device_test, PremultipliedEncodingAndARGBOrderMatchCPUReference)
{
    auto source = device->create_texture({.width = 1, .height = 1}, format_e::rgba_unorm16);
    auto target = device->create_texture({.width = 1, .height = 1}, format_e::rgba_unorm8);
    auto output = device->create_buffer(4, host_access_e::readback);

    auto record = device->try_record();
    record->clear(source, {.125f, .25f, .5f, .5f});
    record->draw(source,
                 target,
                 {.compositing  = compositing_e::replace,
                  .transfer     = color_operation_e::encode_rec709_premultiplied,
                  .output_order = channel_order_e::argb});
    record->readback(target, output);
    finish(record);

    const auto                  actual = output.readable_bytes();
    const std::array<double, 4> expected{
        127.5, (1.099 * std::pow(.25, .45) - .099) * 127.5, (1.099 * std::pow(.5, .45) - .099) * 127.5, 127.5};
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_NEAR(std::to_integer<unsigned>(actual[i]), expected.at(i), 1);
    }
}

TEST_F(device_test, ForeignDeviceResourcesAreRejectedBeforeNativeUse)
{
    device_options_s options;
    // Test options are read from an environment that the test does not modify.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    options.validation = std::getenv("MIXIMUS_VULKAN_VALIDATION") != nullptr;
    device_s other(options);
    auto     image = other.create_texture({.width = 2, .height = 2}, format_e::rgba_unorm8);
    auto     input = device->create_buffer(16, host_access_e::sequential_write);

    auto record = device->try_record();
    EXPECT_THROW(record->upload(input, image), std::invalid_argument);
    record.reset();
    auto foreign = other.try_record();
    foreign->clear(image);
    finish(foreign);
    EXPECT_EQ(other.validation_errors(), 0U);
}

TEST_F(device_test, ReadWriteHostAccessPreservesDirtyBytesAndPartialReadbackPadding)
{
    auto buffer = device->create_buffer(128, host_access_e::read_write);
    std::ranges::fill(buffer.writable_bytes(), std::byte{0x37});
    EXPECT_EQ(buffer.readable_bytes()[127], std::byte{0x37});
    EXPECT_EQ(buffer.writable_bytes()[127], std::byte{0x37});
    auto image = device->create_texture({.width = 2, .height = 1}, format_e::rgba_unorm8);

    auto record = device->try_record();
    record->clear(image, {1, 0, 0, 1});
    record->readback(image, buffer, 128);
    finish(record);

    const auto bytes = buffer.readable_bytes();
    EXPECT_EQ(bytes[0], std::byte{255});
    EXPECT_EQ(bytes[1], std::byte{0});
    for (size_t i = 8; i < bytes.size(); ++i) {
        EXPECT_EQ(bytes[i], std::byte{0x37});
    }
}

TEST_F(device_test, CompletionWaitHonorsCancellation)
{
    std::stop_source stop;
    stop.request_stop();
    completion_s empty;
    EXPECT_EQ(empty.wait(std::chrono::seconds(5), stop.get_token()), wait_result_e::cancelled);
    EXPECT_EQ(empty.wait(std::chrono::milliseconds(0)), wait_result_e::ready);
}

TEST_F(device_test, DescriptorPagesGrowAndAreReusableAfterAbandonment)
{
    auto source = device->create_texture({.width = 2, .height = 2}, format_e::rgba_unorm8);
    auto target = device->create_texture({.width = 2, .height = 2}, format_e::rgba_unorm8);
    auto output = device->create_buffer(16, host_access_e::readback);

    for (int attempt = 0; attempt < 3; ++attempt) {
        auto record = device->try_record();
        record->clear(source, {1, 0, 0, 1});
        for (int draw = 0; draw < 32; ++draw) {
            record->draw(source, target, {.compositing = compositing_e::replace});
        }
        if (attempt == 0) {
            continue; // Abandon several allocated pages, then reuse the same arena.
        }
        record->readback(target, output);
        finish(record);
        const auto bytes = output.readable_bytes();
        for (size_t pixel = 0; pixel < 4; ++pixel) {
            EXPECT_EQ(bytes[pixel * 4], std::byte{255});
            EXPECT_EQ(bytes[(pixel * 4) + 1], std::byte{0});
        }
    }
}

TEST_F(device_test, ClearOperationsAreNotLimitedByDescriptorPageSize)
{
    auto image  = device->create_texture({.width = 1, .height = 1}, format_e::rgba_unorm8);
    auto output = device->create_buffer(4, host_access_e::readback);
    auto record = device->try_record();
    for (int clear = 0; clear < 256; ++clear) {
        record->clear(image, {1, 0, 0, 1});
    }
    record->readback(image, output);
    finish(record);
    EXPECT_EQ(output.readable_bytes()[0], std::byte{255});
}

TEST_F(device_test, FirstDrawInitializesNewTargetsAndLaterRecordingsPreserveContents)
{
    auto source = device->create_texture({.width = 1, .height = 1}, format_e::rgba_unorm8);
    auto target = device->create_texture({.width = 2, .height = 1}, format_e::rgba_unorm8);
    auto output = device->create_buffer(8, host_access_e::readback);

    auto earlier = device->try_record();
    earlier->clear(source, {1, 0, 0, 1});
    draw_s draw{.compositing = compositing_e::replace};
    draw.clip = {0, 0, 1, 1};
    earlier->draw(source, target, draw);
    earlier->readback(target, output);
    finish(earlier);
    EXPECT_EQ(output.readable_bytes()[0], std::byte{255});
    EXPECT_EQ(output.readable_bytes()[7], std::byte{0});

    earlier = device->try_record();
    earlier->clear(source, {0, 1, 0, 1});
    auto later = device->try_record();
    draw.clip  = {1, 0, 1, 1};
    later->draw(source, target, draw);
    later->readback(target, output);
    const auto first = earlier->submit();
    finish(later);
    EXPECT_TRUE(first.ready());
    const std::array<std::byte, 8> expected{std::byte{255},
                                            std::byte{0},
                                            std::byte{0},
                                            std::byte{255},
                                            std::byte{0},
                                            std::byte{255},
                                            std::byte{0},
                                            std::byte{255}};
    EXPECT_TRUE(std::ranges::equal(output.readable_bytes(), expected));
}

void fill_ndi_bgra_input(std::span<std::byte> bytes, uint32_t width, std::span<const uint8_t> alphas, alpha_mode_e mode)
{
    for (size_t y = 0; y < alphas.size(); ++y) {
        for (size_t x = 0; x < width; ++x) {
            const size_t at = (y * width + x) * 4;
            // BGRA input exercises NDI's receive byte order independently of RGBA output.
            bytes[at]     = std::byte{static_cast<uint8_t>(255 - x)};
            bytes[at + 1] = std::byte{static_cast<uint8_t>((x * 13) % 256)};
            bytes[at + 2] = std::byte{static_cast<uint8_t>(x)};
            bytes[at + 3] = std::byte{alphas[y]};
            if (mode == alpha_mode_e::premultiplied && alphas[y] != 0) {
                for (size_t channel = 0; channel < 3; ++channel) {
                    bytes[at + channel] =
                        std::byte((std::to_integer<unsigned>(bytes[at + channel]) * alphas[y] + 127) / 255);
                }
            }
        }
    }
}

void expect_ndi_round_trip(alpha_mode_e               mode,
                           uint32_t                   width,
                           uint32_t                   height,
                           std::span<const std::byte> original,
                           std::span<const std::byte> actual,
                           std::span<const std::byte> linear_bytes)
{
    for (size_t pixel = 0; pixel < size_t{width} * height; ++pixel) {
        const size_t at    = pixel * 4;
        const auto   alpha = std::to_integer<unsigned>(original[at + 3]);
        EXPECT_EQ(actual[at + 3], mode == alpha_mode_e::ignore ? std::byte{255} : original[at + 3]);
        for (size_t channel = 0; channel < 3; ++channel) {
            const auto   encoded_value = std::to_integer<unsigned>(original[at + 2 - channel]);
            const double opacity       = mode == alpha_mode_e::ignore ? 1.0 : alpha / 255.0;
            double       video         = encoded_value / 255.0;
            if (mode == alpha_mode_e::premultiplied) {
                video = alpha != 0 ? static_cast<double>(encoded_value) / alpha : 0.0;
            }
            const double decoded = video < .081 ? video / 4.5 : std::pow((video + .099) / 1.099, 1 / .45);
            uint16_t     stored{};
            std::memcpy(&stored, linear_bytes.data() + ((at + channel) * 2), sizeof(stored));
            EXPECT_NEAR(stored, decoded * opacity * 65535.0, 2);
            if (alpha == 0 && mode != alpha_mode_e::ignore) {
                EXPECT_EQ(actual[at + channel], std::byte{0});
            } else {
                // At alpha 1/255, UNORM16 premultiplication has at most two
                // encoded code values of error. Opaque RGB must round-trip exactly.
                EXPECT_NEAR(std::to_integer<unsigned>(actual[at + channel]),
                            encoded_value,
                            mode == alpha_mode_e::ignore || alpha == 255 ? 0 : 2);
            }
        }
    }
}

TEST_F(device_test, NdiAlphaModesDecodeAndRoundTripAgainstIndependentReferences)
{
    for (const auto mode : {alpha_mode_e::ignore, alpha_mode_e::straight, alpha_mode_e::premultiplied}) {
        SCOPED_TRACE(static_cast<int>(mode));
        constexpr std::array<uint8_t, 7> alphas{0, 1, 17, 64, 128, 254, 255};
        constexpr uint32_t               width      = 256;
        constexpr uint32_t               height     = alphas.size();
        constexpr size_t                 components = size_t{width} * height * 4;
        auto source  = device->create_texture({.width = width, .height = height}, format_e::rgba_unorm8);
        auto linear  = device->create_texture({.width = width, .height = height}, format_e::rgba_unorm16);
        auto encoded = device->create_texture({.width = width, .height = height}, format_e::rgba_unorm8);
        auto input   = device->create_buffer(components, host_access_e::sequential_write);
        auto working = device->create_buffer(components * 2, host_access_e::readback);
        auto output  = device->create_buffer(components, host_access_e::readback);
        auto bytes   = input.writable_bytes();
        fill_ndi_bgra_input(bytes, width, alphas, mode);

        const std::vector<std::byte> original(bytes.begin(), bytes.end());

        auto record = device->try_record();
        record->upload(input, source);
        record->draw(source,
                     linear,
                     {.compositing = compositing_e::replace,
                      .transfer    = rec709_decode_operation(mode),
                      .input_order = channel_order_e::bgra});
        record->readback(linear, working);
        record->draw(
            linear, encoded, {.compositing = compositing_e::replace, .transfer = rec709_encode_operation(mode)});
        record->readback(encoded, output);
        finish(record);

        const auto actual       = output.readable_bytes();
        const auto linear_bytes = working.readable_bytes();
        expect_ndi_round_trip(mode, width, height, original, actual, linear_bytes);
    }
}

TEST_F(device_test, NdiOutputAlphaModesMatchIndependentLinearInputIncludingIgnore)
{
    constexpr std::array<uint16_t, 7> alphas{0, 1, 257, 4096, 16384, 32768, 65535};
    constexpr size_t                  components = alphas.size() * 4;
    std::array<uint16_t, components>  pixels{};
    for (size_t i = 0; i < alphas.size(); ++i) {
        pixels.at(i * 4)       = alphas.at(i) / 4;
        pixels.at((i * 4) + 1) = alphas.at(i) / 2;
        pixels.at((i * 4) + 2) = alphas.at(i);
        pixels.at((i * 4) + 3) = alphas.at(i);
    }

    // Nonzero hidden RGB at zero alpha must not cause division by zero or leak
    // into straight/premultiplied output. Ignore sends opaque black here too.
    pixels[0]   = 32768;
    auto source = device->create_texture({.width = alphas.size(), .height = 1}, format_e::rgba_unorm16);
    auto target = device->create_texture({.width = alphas.size(), .height = 1}, format_e::rgba_unorm8);
    auto input  = device->create_buffer(sizeof(pixels), host_access_e::sequential_write);
    auto output = device->create_buffer(components, host_access_e::readback);
    std::memcpy(input.writable_bytes().data(), pixels.data(), sizeof(pixels));

    auto record = device->try_record();
    record->upload(input, source);
    finish(record);
    for (const auto mode : {alpha_mode_e::ignore, alpha_mode_e::straight, alpha_mode_e::premultiplied}) {
        SCOPED_TRACE(static_cast<int>(mode));
        record = device->try_record();
        record->draw(
            source, target, {.compositing = compositing_e::replace, .transfer = rec709_encode_operation(mode)});
        record->readback(target, output);
        finish(record);

        const auto actual = output.readable_bytes();
        for (size_t i = 0; i < alphas.size(); ++i) {
            const double alpha = alphas.at(i) / 65535.0;
            EXPECT_NEAR(
                std::to_integer<unsigned>(actual[(i * 4) + 3]), mode == alpha_mode_e::ignore ? 255.0 : alpha * 255, 1);
            for (size_t channel = 0; channel < 3; ++channel) {
                const double linear   = (alphas.at(i) != 0U) ? double(pixels.at((i * 4) + channel)) / alphas.at(i) : 0;
                const double encoded  = linear < .018 ? linear * 4.5 : (1.099 * std::pow(linear, .45)) - .099;
                const double expected = encoded * (mode == alpha_mode_e::premultiplied ? alpha : 1.0) * 255;
                EXPECT_NEAR(std::to_integer<unsigned>(actual[(i * 4) + channel]), expected, 1);
            }
        }
    }
}

TEST_F(device_test, IndependentContextsRenderWhileAProducerHoldsItsRecording)
{
    auto         producer_context = device->create_recording_context(1);
    auto         producer_image   = device->create_texture({.width = 4, .height = 4}, format_e::rgba_unorm8);
    auto         image            = device->create_texture({.width = 4, .height = 4}, format_e::rgba_unorm8);
    auto         output           = device->create_buffer(64, host_access_e::readback);
    std::latch   producer_recorded(1);
    std::jthread producer([&](const std::stop_token& stop) {
        auto held = producer_context.try_record();
        held->clear(producer_image);
        producer_recorded.count_down();
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });
    producer_recorded.wait();
    EXPECT_EQ(producer_context.try_record(), nullptr);
    for (int frame = 0; frame < 8; ++frame) {
        auto record = device->try_record();
        ASSERT_TRUE(record);
        record->clear(image, {1, 0, 0, 1});
        record->readback(image, output);
        finish(record);
        EXPECT_EQ(output.readable_bytes()[0], std::byte{255});
    }
}

TEST_F(device_test, ConcurrentRecordingsResolveLayoutsInSubmissionOrder)
{
    auto context = device->create_recording_context(2);
    auto image   = device->create_texture({.width = 4, .height = 4}, format_e::rgba_unorm8);
    auto output  = device->create_buffer(64, host_access_e::readback);
    auto later   = context.try_record();
    auto earlier = context.try_record();
    ASSERT_TRUE(later);
    ASSERT_TRUE(earlier);
    later->clear(image, {0, 0, 1, 1});
    later->readback(image, output);
    earlier->clear(image, {1, 0, 0, 1});
    const auto first = earlier->submit();
    finish(later);
    EXPECT_TRUE(first.ready());
    const auto bytes = output.readable_bytes();
    EXPECT_EQ(bytes[0], std::byte{0});
    EXPECT_EQ(bytes[2], std::byte{255});
}

TEST_F(device_test, PendingDependencyDefersOnlyItsConsumerContext)
{
    using namespace std::chrono_literals;
    struct pause_s
    {
        std::binary_semaphore entered{0};
        std::binary_semaphore resume{0};
        std::atomic_bool      independent_ran_before_producer{};
    };
    auto pause               = std::make_shared<pause_s>();
    auto producer_context    = device->create_recording_context(2);
    auto independent_context = device->create_recording_context(1);
    auto image               = device->create_texture({1, 1}, format_e::rgba_unorm8);
    auto output              = device->create_buffer(4, host_access_e::readback);
    auto blocker             = device->try_record();
    auto consumer            = device->try_record();
    auto warmup              = producer_context.try_record();
    auto producer            = producer_context.try_record();
    auto independent         = independent_context.try_record();
    ASSERT_TRUE(blocker && consumer && warmup && producer && independent);
    producer->clear(image, {1, 0, 0, 1});
    consumer->readback(image, output);
    blocker->on_submitted([pause](const completion_s&) {
        pause->entered.release();
        EXPECT_TRUE(pause->resume.try_acquire_for(3s));
    });
    const auto blocked = blocker->submit();
    ASSERT_TRUE(pause->entered.try_acquire_for(3s));

    // The producer has two queued batches. With one batch per context per pass,
    // its actual image write must remain pending while independent work progresses.
    const auto warmed   = warmup->submit();
    const auto produced = producer->submit();
    EXPECT_FALSE(produced.submitted());
    consumer->wait_for(produced);
    const auto consumed = consumer->submit();
    independent->on_submitted([pause, produced](const completion_s&) {
        pause->independent_ran_before_producer.store(!produced.submitted());
    });
    const auto independent_done = independent->submit();
    pause->resume.release();

    EXPECT_EQ(blocked.wait(3s), wait_result_e::ready);
    EXPECT_EQ(warmed.wait(3s), wait_result_e::ready);
    EXPECT_EQ(independent_done.wait(3s), wait_result_e::ready);
    EXPECT_TRUE(pause->independent_ran_before_producer.load());
    ASSERT_EQ(consumed.wait(3s), wait_result_e::ready);
    EXPECT_EQ(output.readable_bytes()[0], std::byte{255});
    EXPECT_EQ(output.readable_bytes()[1], std::byte{0});
}

TEST_F(device_test, PublicationRunsAfterSubmissionAndAbortedCapturesAreReleased)
{
    std::atomic_bool   published{};
    const auto         render_thread = std::this_thread::get_id();
    std::atomic_bool   publication_on_worker{};
    std::weak_ptr<int> abandoned_lease;
    {
        auto record     = device->try_record();
        auto lease      = std::make_shared<int>(1);
        abandoned_lease = lease;
        record->on_submitted([lease, &published](const completion_s&) { published.store(true); });
    }
    EXPECT_TRUE(abandoned_lease.expired());
    EXPECT_FALSE(published.load());
    auto record = device->try_record();
    record->on_submitted([&](const completion_s& completion) {
        EXPECT_EQ(completion.wait_submitted(std::chrono::milliseconds(0)), wait_result_e::ready);
        publication_on_worker.store(std::this_thread::get_id() != render_thread);
        published.store(true);
    });
    finish(record);
    EXPECT_TRUE(published.load());
    EXPECT_TRUE(publication_on_worker.load());
}
}} // namespace miximus::gpu
