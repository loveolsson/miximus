#include "font_loader.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

namespace miximus::render::font {

font_loader_s::font_loader_s()
{
    auto res = FT_Init_FreeType(reinterpret_cast<FT_Library*>(&library_));
    if (res != 0) {
        throw std::runtime_error("Failed to initialize FreeType");
    }
}

font_loader_s::~font_loader_s() { FT_Done_FreeType(reinterpret_cast<FT_Library>(&library_)); }

std::unique_ptr<font_instance_s> font_loader_s::load_font(const font_variant_s* face)
{
    if (!face) {
        return nullptr;
    }

    auto font = std::make_unique<font_instance_s>(library_, face->path, face->index);
    if (font->valid_) {
        return font;
    }

    return nullptr;
}

} // namespace miximus::render::font
