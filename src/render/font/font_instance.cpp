#include "font_instance.hpp"

#include <ft2build.h>
#include FT_FREETYPE_H

namespace miximus::render::font {

font_instance_s::font_instance_s(void* l, const std::filesystem::path& path, int index)
{
    auto* library = reinterpret_cast<FT_Library>(l);
    auto  error   = FT_New_Face(library, path.c_str(), index, reinterpret_cast<FT_Face*>(&face));
    if (error == 0) {
        valid_ = true;
    }
}

font_instance_s::~font_instance_s()
{
    if (face != nullptr) {
        FT_Done_Face(reinterpret_cast<FT_Face>(face));
    }
}

void font_instance_s::set_size(int size_in_px) { FT_Set_Pixel_Sizes(reinterpret_cast<FT_Face>(face), 0, size_in_px); }

} // namespace miximus::render::font
