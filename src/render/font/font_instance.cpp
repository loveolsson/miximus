#include "font_instance.hpp"

#include "render/font/font_loader.hpp"
#include "render/surface/surface.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <glm/common.hpp>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include FT_COLOR_H
#include FT_OUTLINE_H

namespace miximus::render {

namespace {
int floor_pixel(FT_Pos value) { return static_cast<int>(std::floor(static_cast<double>(value) / 64.0)); }
int ceil_pixel(FT_Pos value) { return static_cast<int>(std::ceil(static_cast<double>(value) / 64.0)); }

void check_freetype(FT_Error error)
{
    if (error != 0) {
        throw std::runtime_error(std::format("FreeType operation failed: {}", error));
    }
}

bool is_wrap_space(char32_t codepoint) { return codepoint == U' ' || codepoint == U'\t'; }

size_t consume_line_separator(std::u32string_view str, size_t offset)
{
    if (offset < str.size() && (str[offset] == U'\r' || str[offset] == U'\n')) {
        const bool is_carriage_return = str[offset] == U'\r';
        ++offset;
        if (is_carriage_return && offset < str.size() && str[offset] == U'\n') {
            ++offset;
        }
    }
    return offset;
}

size_t consume_wrap_separator(std::u32string_view str, size_t offset)
{
    while (offset < str.size() && is_wrap_space(str[offset])) {
        ++offset;
    }
    // A forced wrap immediately before a newline is still just one line.
    return consume_line_separator(str, offset);
}

void include_ink(font_instance_s::line_metrics_s& metrics, gpu::recti_s bounds)
{
    if (bounds.size.x <= 0 || bounds.size.y <= 0) {
        return;
    }
    if (!metrics.has_ink) {
        metrics.ink_bounds = bounds;
        metrics.has_ink    = true;
        return;
    }
    const auto minimum = glm::min(metrics.ink_bounds.pos, bounds.pos);
    const auto maximum = glm::max(metrics.ink_bounds.pos + metrics.ink_bounds.size, bounds.pos + bounds.size);
    metrics.ink_bounds = {.pos = minimum, .size = maximum - minimum};
}
} // namespace

font_instance_s::font_instance_s(std::shared_ptr<font_loader_s> loader, const std::filesystem::path& path, int index)
    : loader_(std::move(loader))
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        return;
    }

    const auto size = file.tellg();
    if (size <= 0 || size > std::numeric_limits<FT_Long>::max()) {
        return;
    }

    file_data_.resize(static_cast<size_t>(size));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(file_data_.data()), static_cast<std::streamsize>(file_data_.size()));
    if (!file) {
        file_data_.clear();
        return;
    }

    auto error = FT_New_Memory_Face(
        loader_->library_, file_data_.data(), static_cast<FT_Long>(file_data_.size()), index, &face_);
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

void font_instance_s::set_size(int size_in_px)
{
    if (!valid_ || size_in_px <= 0) {
        throw std::invalid_argument("Font size requires a valid face and positive pixels");
    }
    if (pixel_size_ == size_in_px) {
        return;
    }
    check_freetype(FT_Set_Pixel_Sizes(face_, static_cast<FT_UInt>(size_in_px), static_cast<FT_UInt>(size_in_px)));
    pixel_size_ = size_in_px;
}

int font_instance_s::line_height() const { return std::max(1, ceil_pixel(face_->size->metrics.height)); }

font_instance_s::glyph_metrics_s font_instance_s::load_glyph(FT_UInt index, bool render_bitmap)
{
    const auto flags = static_cast<FT_Int32>(FT_LOAD_COLOR | (render_bitmap ? 0 : FT_LOAD_BITMAP_METRICS_ONLY));
    check_freetype(FT_Load_Glyph(face_, index, flags));

    const auto      slot = face_->glyph;
    glyph_metrics_s result;
    result.advance = {slot->advance.x / 64, slot->advance.y / 64};

    FT_LayerIterator layers{};
    FT_UInt          layer_glyph{};
    FT_UInt          layer_color{};
    const bool has_color_layers = FT_Get_Color_Glyph_Layer(face_, index, &layer_glyph, &layer_color, &layers) != 0;

    if (slot->format == FT_GLYPH_FORMAT_OUTLINE && !has_color_layers) {
        // The grid-fitted control box bounds the normal rasterizer's bitmap,
        // without rasterizing an outline during measurement.
        FT_BBox box{};
        FT_Outline_Get_CBox(&slot->outline, &box);
        const gpu::vec2i_t minimum{floor_pixel(box.xMin), -ceil_pixel(box.yMax)};
        const gpu::vec2i_t maximum{ceil_pixel(box.xMax), -floor_pixel(box.yMin)};
        result.bounds = {.pos = minimum, .size = maximum - minimum};
    } else if (slot->format == FT_GLYPH_FORMAT_BITMAP) {
        result.bounds.pos  = {slot->bitmap_left, -slot->bitmap_top};
        result.bounds.size = {slot->bitmap.width, slot->bitmap.rows};
    } else {
        // Formats without usable outline/bitmap metrics require rasterization.
        if (!render_bitmap) {
            check_freetype(FT_Load_Glyph(face_, index, FT_LOAD_COLOR));
        }
        check_freetype(FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL));
        result.bounds.pos  = {slot->bitmap_left, -slot->bitmap_top};
        result.bounds.size = {slot->bitmap.width, slot->bitmap.rows};
    }

    if (render_bitmap && slot->format != FT_GLYPH_FORMAT_BITMAP) {
        check_freetype(FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL));
    }
    return result;
}

void font_instance_s::paint_glyph(surface_s& surface, gpu::vec2i_t position)
{
    const auto  slot   = face_->glyph;
    const auto& bitmap = slot->bitmap;
    if (bitmap.width == 0 || bitmap.rows == 0) {
        return;
    }
    const gpu::vec2i_t dimensions{bitmap.width, bitmap.rows};
    position += gpu::vec2i_t{slot->bitmap_left, -slot->bitmap_top};

    if (bitmap.pixel_mode == FT_PIXEL_MODE_BGRA) {
        const auto* pixels = reinterpret_cast<const srgb_premultiplied_bgra_pixel_s*>(bitmap.buffer);
        const auto  source =
            strided_image_view_s<srgb_premultiplied_bgra_pixel_s>::from_rows(pixels, dimensions, bitmap.pitch);
        surface.source_over(source, position);
    } else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
        const auto* pixels = reinterpret_cast<const coverage_pixel_t*>(bitmap.buffer);
        const auto  source = strided_image_view_s<coverage_pixel_t>::from_rows(pixels, dimensions, bitmap.pitch);
        surface.source_over(source, position);
    } else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
        // Embedded monochrome strikes need expansion to the surface's 8-bit coverage.
        std::vector<coverage_pixel_t> coverage(static_cast<size_t>(bitmap.width) * bitmap.rows);
        for (size_t y = 0; y < bitmap.rows; ++y) {
            const auto* row = bitmap.buffer + (static_cast<ptrdiff_t>(y) * bitmap.pitch);
            for (size_t x = 0; x < bitmap.width; ++x) {
                const auto mask                  = 0x80U >> (x % 8);
                const bool covered               = (row[x / 8] & mask) != 0;
                coverage[(y * bitmap.width) + x] = covered ? 255 : 0;
            }
        }
        const auto source =
            strided_image_view_s<coverage_pixel_t>::from_rows(coverage.data(), dimensions, bitmap.width);
        surface.source_over(source, position);
    } else {
        throw std::runtime_error("Unsupported font bitmap pixel mode");
    }
}

font_instance_s::line_layout_s font_instance_s::layout_line(std::u32string_view str,
                                                            std::optional<int>  width,
                                                            surface_s*          surface,
                                                            gpu::vec2i_t        position)
{
    if (pixel_size_ <= 0) {
        throw std::logic_error("Set font size before laying out text");
    }

    line_layout_s result;
    line_layout_s word_break;
    FT_UInt       previous_index{};
    for (size_t i = 0; i < str.size(); ++i) {
        const auto codepoint = str[i];
        if (codepoint == U'\r' || codepoint == U'\n') {
            result.consumed_length = consume_line_separator(str, i);
            return result;
        }

        if (width && is_wrap_space(codepoint)) {
            if (i == 0 || !is_wrap_space(str[i - 1])) {
                word_break = result;
            }
            word_break.consumed_length = i + 1;
        }

        const auto glyph_index = FT_Get_Char_Index(face_, codepoint == U'\t' ? U' ' : codepoint);
        const auto glyph       = load_glyph(glyph_index, surface != nullptr);
        FT_Vector  kerning{};
        check_freetype(FT_Get_Kerning(face_, previous_index, glyph_index, FT_KERNING_DEFAULT, &kerning));

        const gpu::vec2i_t kerning_offset{kerning.x / 64, kerning.y / 64};
        auto               next_line      = result;
        const auto         glyph_position = result.metrics.advance + kerning_offset;
        const gpu::recti_s ink_bounds{
            .pos  = glyph_position + glyph.bounds.pos,
            .size = glyph.bounds.size,
        };
        include_ink(next_line.metrics, ink_bounds);
        next_line.metrics.advance += kerning_offset + glyph.advance;
        next_line.text_length     = i + 1;
        next_line.consumed_length = i + 1;

        if (width && next_line.metrics.advance.x > std::max(0, *width)) {
            // Split an overlong first word; guarantee forward progress.
            auto wrapped = i == 0 ? next_line : result;
            if (word_break.text_length > 0) {
                wrapped = word_break;
            }
            wrapped.consumed_length = consume_wrap_separator(str, wrapped.consumed_length);
            return wrapped;
        }

        if (surface != nullptr) {
            paint_glyph(*surface, position + glyph_position);
        }
        result         = next_line;
        previous_index = glyph_index;
    }
    return result;
}

font_instance_s::line_layout_s font_instance_s::measure_line(std::u32string_view str)
{
    return layout_line(str, std::nullopt, nullptr, {});
}

font_instance_s::line_layout_s font_instance_s::flow_line(std::u32string_view str, int width)
{
    return layout_line(str, width, nullptr, {});
}

font_instance_s::line_layout_s
font_instance_s::render_line(std::u32string_view str, surface_s* surface, gpu::vec2i_t baseline)
{
    return layout_line(str, std::nullopt, surface, baseline);
}

} // namespace miximus::render
