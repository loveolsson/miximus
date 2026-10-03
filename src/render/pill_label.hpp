#pragma once

#include "gpu/types.hpp"
#include "render/font/font_instance_fwd.hpp"
#include "render/surface/surface_fwd.hpp"

#include <string>
#include <string_view>

namespace miximus::render {

struct pill_style_s
{
    int font_size{};
    int padding_x{};
    int padding_y{};
    int max_width{};
    int max_height{};

    bool operator==(const pill_style_s& other) const = default;
};

struct pill_label_s
{
    std::u32string text;
    int            font_size{};
    gpu::vec2i_t   dimensions{};
    gpu::vec2i_t   baseline{};
};

// CPU-worker operations. The caller owns the font and surface storage.
pill_label_s make_pill_label(font_instance_s& font, std::string_view text, const pill_style_s& style);
void         render_pill_label(font_instance_s& font, const pill_label_s& label, surface_s& surface);

} // namespace miximus::render
