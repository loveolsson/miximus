#include "image_file.hpp"

#include "detail/color_lut.hpp"
#include "utils/filesystem.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

namespace miximus::render {

stb::decoded_image_s load_image_file(const std::string& path_utf8)
{
    std::ifstream file(utils::path_from_utf8(path_utf8), std::ios::binary | std::ios::ate);
    if (!file) {
        throw std::runtime_error("Cannot open image file");
    }
    const auto               length         = file.tellg();
    constexpr std::streamoff MAX_FILE_BYTES = 256LL * 1024 * 1024;
    if (length <= 0 || length > MAX_FILE_BYTES) {
        throw std::runtime_error("Image file must be nonempty and no larger than 256 MiB");
    }
    std::vector<std::byte> encoded(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(encoded.size()))) {
        throw std::runtime_error("Cannot read image file");
    }
    auto image = stb::decode_image(encoded, stb::image_channels_e::rgba);
    if (image.width() > 16384 || image.height() > 16384) {
        throw std::runtime_error("Image dimensions exceed 16384 pixels");
    }
    auto pixels = image.pixels();
    for (size_t offset = 0; offset < pixels.size(); offset += 4) {
        const auto alpha = static_cast<unsigned>(pixels[offset + 3]);
        for (size_t channel = 0; channel < 3; ++channel) {
            const auto linear        = static_cast<unsigned>(detail::SRGB_TO_LINEAR_U8.at(pixels[offset + channel]));
            pixels[offset + channel] = static_cast<uint8_t>((linear * alpha + 127U) / 255U);
        }
    }
    return image;
}

} // namespace miximus::render
