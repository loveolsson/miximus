#include "static_files/files.hpp"
#include "stb_image.h"
#include "surface.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace miximus::render {
namespace {
struct asset_s
{
    gpu::vec2i_t                         dimensions;
    std::vector<surface_s::rgba_pixel_t> pixels;
};

struct stbi_deleter_s
{
    void operator()(stbi_uc* pixels) const { stbi_image_free(pixels); }
};

uint8_t rec709_to_linear(uint8_t value)
{
    const double encoded = static_cast<double>(value) / 255.0;
    const double linear  = encoded < 0.081 ? encoded / 4.5 : std::pow((encoded + 0.099) / 1.099, 1.0 / 0.45);
    return static_cast<uint8_t>(std::lround(std::clamp(linear, 0.0, 1.0) * 255.0));
}

std::shared_ptr<const asset_s> load_asset(std::string_view resource_path)
{
    static std::mutex                                                         mutex;
    static std::map<std::string, std::shared_ptr<const asset_s>, std::less<>> assets;

    const std::scoped_lock lock(mutex);
    if (const auto existing = assets.find(resource_path); existing != assets.end()) {
        return existing->second;
    }

    const auto file_data = static_files::get_resource_files().get_file_or_throw(resource_path).unzip();
    if (file_data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        throw std::length_error(std::format("Image {} is too large to decode", resource_path));
    }

    gpu::vec2i_t                                   dimensions;
    int                                            source_channels{};
    const std::unique_ptr<stbi_uc, stbi_deleter_s> decoded(
        stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(file_data.data()),
                              static_cast<int>(file_data.size()),
                              &dimensions.x,
                              &dimensions.y,
                              &source_channels,
                              4));
    if (!decoded) {
        throw std::runtime_error(std::format("Failed to load image {}", resource_path));
    }
    constexpr size_t channel_count = 4;
    if (dimensions.x <= 0 || dimensions.y <= 0 ||
        static_cast<size_t>(dimensions.x) > std::numeric_limits<size_t>::max() / static_cast<size_t>(dimensions.y) ||
        (static_cast<size_t>(dimensions.x) * static_cast<size_t>(dimensions.y)) >
            std::numeric_limits<size_t>::max() / channel_count) {
        throw std::length_error(std::format("Image {} has invalid dimensions", resource_path));
    }

    const auto pixel_count = static_cast<size_t>(dimensions.x) * static_cast<size_t>(dimensions.y);
    auto       asset       = std::make_shared<asset_s>(asset_s{
                    .dimensions = dimensions,
                    .pixels     = std::vector<surface_s::rgba_pixel_t>(pixel_count),
    });
    for (size_t i = 0; i < pixel_count; ++i) {
        const size_t offset = i * channel_count;
        asset->pixels[i]    = {
            rec709_to_linear(decoded.get()[offset]),
            rec709_to_linear(decoded.get()[offset + 1]),
            rec709_to_linear(decoded.get()[offset + 2]),
            decoded.get()[offset + 3],
        };
    }

    const auto result = assets.emplace(std::string(resource_path), std::move(asset));
    return result.first->second;
}
} // namespace

gpu::vec2i_t surface_s::asset_dimensions(std::string_view resource_path)
{
    return load_asset(resource_path)->dimensions;
}

void surface_s::draw_asset(std::string_view resource_path, gpu::vec2i_t position)
{
    const auto asset = load_asset(resource_path);
    source_over(asset->pixels.data(),
                asset->dimensions,
                sizeof(rgba_pixel_t) * static_cast<size_t>(asset->dimensions.x),
                position);
}

} // namespace miximus::render
