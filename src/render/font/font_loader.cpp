#include "font_loader.hpp"

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

    auto font = std::make_unique<font_instance_s>(library_, face->path, face->index);
    if (font->valid_) {
        return font;
    }

    return nullptr;
}

} // namespace miximus::render
