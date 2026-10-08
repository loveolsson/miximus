#include "render/image_file.hpp"
#include "utils/filesystem.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <string>
#include <system_error>

namespace {
using namespace miximus;

class ImageFile : public testing::Test
{
    std::filesystem::path path_;

  protected:
    const std::filesystem::path& path() const { return path_; }

    void SetUp() override
    {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_             = std::filesystem::temp_directory_path() /
                            utils::path_from_utf8("miximus-image-\xC3\xA5-" + std::to_string(unique) + ".tga");
    }

    void TearDown() override
    {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }
};

TEST_F(ImageFile, DecodesUnicodePathAndConvertsSrgbToLinearPremultipliedRgba)
{
    // Three top-down BGRA pixels: opaque red, half-alpha mid-red, transparent white.
    const std::array<unsigned char, 30> tga{
        0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 1, 0, 32, 40, 0, 0, 255, 255, 0, 0, 128, 128, 255, 255, 255, 0,
    };
    {
        std::ofstream file(path(), std::ios::binary);
        file.write(reinterpret_cast<const char*>(tga.data()), tga.size());
        ASSERT_TRUE(file);
    }
    const auto image = render::load_image_file(utils::path_to_utf8(path()));
    EXPECT_EQ(image.width(), 3);
    EXPECT_EQ(image.height(), 1);
    const std::array<unsigned char, 12> expected{255, 0, 0, 255, 28, 0, 0, 128, 0, 0, 0, 0};
    ASSERT_EQ(image.pixels().size(), expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(image.pixels()[i], expected.at(i)) << i;
    }
}

TEST_F(ImageFile, MissingEmptyAndInvalidFilesReportErrors)
{
    const auto name = utils::path_to_utf8(path());
    EXPECT_THROW((void)render::load_image_file(name), std::runtime_error);
    {
        const std::ofstream file{path()};
    }
    EXPECT_THROW((void)render::load_image_file(name), std::runtime_error);
    {
        std::ofstream file{path()};
        file << "not an image";
    }
    EXPECT_THROW((void)render::load_image_file(name), std::runtime_error);
}
} // namespace
