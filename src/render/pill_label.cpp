#include "render/pill_label.hpp"

#include "render/font/font_instance.hpp"
#include "render/surface/surface.hpp"
#include "utils/string_utils.hpp"

#include <algorithm>
#include <utility>

namespace miximus::render {

pill_label_s make_pill_label(font_instance_s& font, std::string_view text, const pill_style_s& style)
{
    if (text.empty() || style.font_size <= 0 || style.padding_x < 0 || style.padding_y < 0 ||
        style.max_width <= 2 * style.padding_x || style.max_height <= 2 * style.padding_y) {
        return {};
    }

    font.set_size(style.font_size);
    auto line = utils::utf8_to_utf32(text);
    for (auto& c : line) {
        if (c == U'\n' || c == U'\r' || c == U'\t') {
            c = U' ';
        }
    }

    const int available_width = style.max_width - (2 * style.padding_x);
    auto      bounds          = font.measure_line(line).metrics.ink_bounds;
    if (bounds.size.x > available_width) {
        // Binary search keeps very long source names from requiring quadratic glyph work.
        size_t low  = 0;
        size_t high = line.size();
        while (low < high) {
            const size_t middle    = low + ((high - low + 1) / 2);
            const auto   candidate = line.substr(0, middle) + U'\u2026';
            if (font.measure_line(candidate).metrics.ink_bounds.size.x <= available_width) {
                low = middle;
            } else {
                high = middle - 1;
            }
        }
        line   = line.substr(0, low) + U'\u2026';
        bounds = font.measure_line(line).metrics.ink_bounds;
    }

    if (bounds.size.x <= 0 || bounds.size.y <= 0 || bounds.size.x > available_width ||
        bounds.size.y + (2 * style.padding_y) > style.max_height) {
        return {};
    }

    const int height = bounds.size.y + (2 * style.padding_y);
    const int width  = std::max(height, bounds.size.x + (2 * style.padding_x));
    if (width > style.max_width) {
        return {};
    }
    const gpu::vec2i_t text_position{(width - bounds.size.x) / 2, style.padding_y};
    return {
        .text       = std::move(line),
        .font_size  = style.font_size,
        .dimensions = {width, height},
        .baseline   = text_position - bounds.pos,
    };
}

void render_pill_label(font_instance_s& font, const pill_label_s& label, surface_s& surface)
{
    font.set_size(label.font_size);
    surface.clear({0, 0, 0, 0});
    const gpu::recti_s pill_bounds{
        .pos  = {0, 0},
        .size = label.dimensions,
    };
    surface.fill_pill(pill_bounds, {0, 0, 0, 191});
    font.render_line(label.text, &surface, label.baseline);
}

} // namespace miximus::render
