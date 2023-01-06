#include "font_instance.hpp"
#include "render/font/font_loader.hpp"
#include "render/surface/surface.hpp"

#include <cwctype>

namespace miximus::render {

font_instance_s::font_instance_s(std::shared_ptr<font_loader_s> loader, const std::filesystem::path& path, int index)
    : loader_(std::move(loader))
{
    auto error = FT_New_Face(loader_->library_, path.u8string().c_str(), index, &face_);
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

void font_instance_s::set_size(int size_in_px) { FT_Set_Pixel_Sizes(face_, size_in_px, size_in_px); }

font_instance_s::flow_info_s font_instance_s::flow_line(std::u32string_view str, int width)
{
    flow_info_s info = {};

    size_t      word_len    = 0;
    FT_UInt     prior_index = 0;
    const auto* slot        = face_->glyph;

    for (int i = 0; i < str.size(); ++i) {
        char32_t c = str[i];

        if (std::iswspace(c) != 0 || i == str.size() - 1) {
            if (info.pixels_advanced + word_len < width) {
                info.pixels_advanced += word_len;
                word_len            = 0;
                info.consumed_chars = i + 1;
            } else {
                break;
            }
        }

        if (c == U'\r') {
            continue;
        }

        if (c == U'\n') {
            info.pixels_advanced += word_len;
            info.consumed_chars = i + 1;
            break;
        }

        auto index = FT_Get_Char_Index(face_, c);
        FT_Load_Glyph(face_, index, FT_LOAD_BITMAP_METRICS_ONLY);

        FT_Vector kerning = {};
        if (i > 0) {
            FT_Get_Kerning(face_, prior_index, index, FT_KERNING_DEFAULT, &kerning);
        }
        prior_index = index;

        word_len += static_cast<size_t>(slot->advance.x + kerning.x) >> 6U;

        if (word_len >= width) {
            if (info.consumed_chars == 0) {
                // Accept word if it's longer than the line, and it's the first word
                info.consumed_chars = i;
            }
            break;
        }
    }

    return info;
}

gpu::vec2i_t font_instance_s::render_string(std::u32string_view str, surface_s* surface, gpu::vec2i_t pos)
{
    const auto* slot        = face_->glyph;
    FT_UInt     prior_index = 0;

    for (char32_t c : str) {
        if (c == U'\r' || c == U'\n') {
            continue;
        }

        auto index = FT_Get_Char_Index(face_, c);
        auto error = FT_Load_Glyph(face_, index, FT_LOAD_COLOR);
        if (error != 0) {
            continue;
        }

        error = FT_Render_Glyph(face_->glyph, FT_RENDER_MODE_NORMAL);
        if (error != 0) {
            continue;
        }

        FT_Vector kerning = {};
        FT_Get_Kerning(face_, prior_index, index, FT_KERNING_DEFAULT, &kerning);
        prior_index = index;

        constexpr int div = 0x40;

        const FT_Bitmap& bitmap = slot->bitmap;
        gpu::vec2i_t     offset{slot->bitmap_left + (kerning.x / div), -slot->bitmap_top + (kerning.y / div)};

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

        pos.x += static_cast<int>((slot->advance.x + kerning.x) / div);
        pos.y += static_cast<int>((slot->advance.y + kerning.y) / div);
    }

    return pos;
}

} // namespace miximus::render
