#pragma once
#include "gpu/types.hpp"
#include "render/surface/surface_fwd.hpp"

#include <memory>
#include <string_view>

namespace miximus::render {

class image_asset_s;

std::shared_ptr<const image_asset_s> load_image_asset(std::string_view resource_path);
gpu::vec2i_t                         image_asset_dimensions(const image_asset_s& asset);
void draw_image_asset(surface_s& surface, const image_asset_s& asset, gpu::vec2i_t position);

} // namespace miximus::render
