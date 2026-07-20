#include "image_asset.hpp"

#include "render/surface/surface.hpp"
#include "static_files/files.hpp"
#include "wrapper/stb/image.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace miximus::render {

class image_asset_s
{
  public:
    gpu::vec2i_t                         dimensions;
    std::vector<surface_s::rgba_pixel_t> pixels;
};

namespace {
uint8_t rec709_to_linear(uint8_t value)
{
    const double encoded = static_cast<double>(value) / 255.0;
    const double linear  = encoded < 0.081 ? encoded / 4.5 : std::pow((encoded + 0.099) / 1.099, 1.0 / 0.45);
    return static_cast<uint8_t>(std::lround(std::clamp(linear, 0.0, 1.0) * 255.0));
}

std::shared_ptr<const image_asset_s> create_image_asset(std::string_view resource_path)
{
    const auto encoded     = static_files::get_resource_files().get_file_or_throw(resource_path).unzip();
    const auto image       = stb::decode_image(std::as_bytes(std::span{encoded}), stb::image_channels_e::rgba);
    const auto dimensions  = gpu::vec2i_t{image.width(), image.height()};
    const auto pixel_count = static_cast<size_t>(dimensions.x) * static_cast<size_t>(dimensions.y);

    auto asset = std::make_shared<image_asset_s>(image_asset_s{
        .dimensions = dimensions,
        .pixels     = std::vector<surface_s::rgba_pixel_t>(pixel_count),
    });
    for (size_t i = 0; i < pixel_count; ++i) {
        constexpr size_t channel_count = 4;
        const size_t     offset        = i * channel_count;
        asset->pixels[i]               = {
            rec709_to_linear(image.data()[offset]),
            rec709_to_linear(image.data()[offset + 1]),
            rec709_to_linear(image.data()[offset + 2]),
            image.data()[offset + 3],
        };
    }
    return asset;
}
} // namespace

std::shared_ptr<const image_asset_s> load_image_asset(std::string_view resource_path)
{
    return create_image_asset(resource_path);
}

gpu::vec2i_t image_asset_dimensions(const image_asset_s& asset) { return asset.dimensions; }

void draw_image_asset(surface_s& surface, const image_asset_s& asset, gpu::vec2i_t position)
{
    surface.source_over(asset.pixels.data(),
                        asset.dimensions,
                        sizeof(surface_s::rgba_pixel_t) * static_cast<size_t>(asset.dimensions.x),
                        position);
}

} // namespace miximus::render
