#include "font_instance.hpp"
#include "render/surface/surface.hpp"

#include <ft2build.h>

#include FT_FREETYPE_H

#include <cwctype>

namespace miximus::render {

font_instance_s::font_instance_s(FT_Library library, const std::filesystem::path& path, int index)
{
    auto error = FT_New_Face(library, path.c_str(), index, &face_);
    if (error == 0) {
        valid_ = true;
    }
}

font_instance_s::~font_instance_s()
{
    if (face_ != nullptr) {
        FT_Done_Face(face_);
    }
}

void font_instance_s::set_size(int size_in_px) { FT_Set_Pixel_Sizes(face_, 0, size_in_px); }

size_t font_instance_s::fit_line(std::u32string_view str, int width)
{
    size_t consumed = 0;

    size_t      cursor   = 0;
    size_t      word_len = 0;
    const auto* slot     = face_->glyph;

    for (int i = 0; i < str.size(); ++i) {
        char32_t c = str[i];

        if (std::iswspace(c) != 0 || i == str.size() - 1) {
            if (cursor + word_len < width) {
                cursor += word_len;
                word_len = 0;
                consumed = i + 1;
            } else {
                break;
            }
        }

        auto index = FT_Get_Char_Index(face_, c);
        FT_Load_Glyph(face_, index, FT_LOAD_BITMAP_METRICS_ONLY);

        word_len += static_cast<size_t>(slot->advance.x) >> 6U;

        if (word_len >= width) {
            if (consumed == 0) {
                // Accept word if it's longer than the line, and it's the first word
                consumed = i;
            }
            break;
        }
    }

    return consumed;
}

size_t font_instance_s::draw_line(std::u32string_view str, surface_s* surface, gpu::vec2i_t pos)
{
    const auto* slot = face_->glyph;

    for (char32_t c : str) {
        auto index = FT_Get_Char_Index(face_, c);
        auto error = FT_Load_Glyph(face_, index, FT_LOAD_COLOR);
        if (error != 0) {
            continue;
        }

        error = FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL);
        if (error != 0) {
            continue;
        }

        const FT_Bitmap& bitmap = slot->bitmap;
        gpu::vec2i_t     offset{slot->bitmap_left, -slot->bitmap_top};

        if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
            surface->alpha_blend(reinterpret_cast<surface_s::rgba_pixel_t*>(bitmap.buffer),
                                 {bitmap.width, bitmap.rows},
                                 bitmap.pitch,
                                 pos + offset);
        } else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
            surface->alpha_blend(reinterpret_cast<surface_s::mono_pixel_t*>(bitmap.buffer),
                                 {bitmap.width, bitmap.rows},
                                 bitmap.pitch,
                                 pos + offset);
        }

        pos.x += static_cast<size_t>(slot->advance.x) >> 6U;
        pos.y += static_cast<size_t>(slot->advance.y) >> 6U;
    }

    return str.size();
}

} // namespace miximus::render
