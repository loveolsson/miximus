#include "image.hpp"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <format>
#include <limits>
#include <stdexcept>

namespace miximus::stb {

namespace detail {
void image_deleter_s::operator()(uint8_t* data) const noexcept { stbi_image_free(data); }
} // namespace detail

decoded_image_s::decoded_image_s(int                                               width,
                                 int                                               height,
                                 int                                               source_channels,
                                 image_channels_e                                  channels,
                                 std::unique_ptr<uint8_t, detail::image_deleter_s> data)
    : width_(width)
    , height_(height)
    , source_channels_(source_channels)
    , channels_(channels)
    , data_(std::move(data))
{
}

size_t decoded_image_s::byte_size() const
{
    return static_cast<size_t>(width_) * static_cast<size_t>(height_) * static_cast<size_t>(channels_);
}

decoded_image_s decode_image(std::span<const std::byte> encoded, image_channels_e channels)
{
    if (encoded.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error("Encoded image is too large for stb_image");
    }

    int output_channels{};
    switch (channels) {
        case image_channels_e::grayscale:
            output_channels = STBI_grey;
            break;
        case image_channels_e::grayscale_alpha:
            output_channels = STBI_grey_alpha;
            break;
        case image_channels_e::rgb:
            output_channels = STBI_rgb;
            break;
        case image_channels_e::rgba:
            output_channels = STBI_rgb_alpha;
            break;
        default:
            throw std::invalid_argument("Invalid decoded image channel count");
    }

    int                                               width{};
    int                                               height{};
    int                                               source_channels{};
    std::unique_ptr<uint8_t, detail::image_deleter_s> data(
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(encoded.data()),
                              static_cast<int>(encoded.size()),
                              &width,
                              &height,
                              &source_channels,
                              output_channels));
    if (!data) {
        const auto* reason = stbi_failure_reason();
        throw std::runtime_error(
            std::format("Failed to decode image: {}", reason != nullptr ? reason : "unknown error"));
    }
    if (width <= 0 || height <= 0 ||
        static_cast<size_t>(width) > std::numeric_limits<size_t>::max() / static_cast<size_t>(height) ||
        (static_cast<size_t>(width) * static_cast<size_t>(height)) >
            std::numeric_limits<size_t>::max() / static_cast<size_t>(channels)) {
        throw std::length_error("Decoded image has invalid dimensions");
    }

    return {width, height, source_channels, channels, std::move(data)};
}

} // namespace miximus::stb
