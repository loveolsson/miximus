#include "font_loader.hpp"

#include "render/font/font_info.hpp"
#include "render/font/font_instance.hpp"
#include "static_files/files.hpp"

#include <memory>
#include <stdexcept>
#include <vector>

namespace miximus::render {

font_loader_s::font_loader_s()
{
    auto res = FT_Init_FreeType(&library_);
    if (res != 0) {
        throw std::runtime_error("Failed to initialize FreeType");
    }
}

font_loader_s::~font_loader_s() { FT_Done_FreeType(library_); }

std::unique_ptr<font_instance_s> font_loader_s::load_font(const font_variant_s* face)
{
    if (face == nullptr) {
        return nullptr;
    }

    std::unique_ptr<font_instance_s> font;
    if (face->resource_path.empty()) {
        font = std::make_unique<font_instance_s>(shared_from_this(), face->path, face->index);
    } else {
        const auto* resource = static_files::get_resource_files().get_file(face->resource_path);
        if (resource == nullptr) {
            return nullptr;
        }
        const auto data = resource->unzip();
        font            = std::make_unique<font_instance_s>(
            shared_from_this(), std::vector<FT_Byte>(data.begin(), data.end()), face->index);
    }
    if (font->valid()) {
        return font;
    }

    return nullptr;
}

} // namespace miximus::render
