#pragma once
#include "gpu/types.hpp"
#include "render/font/font_loader_fwd.hpp"
#include "render/surface/surface_fwd.hpp"

#include <ft2build.h>

#include FT_FREETYPE_H

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

namespace miximus::render {

class font_instance_s
{
  public:
    struct line_metrics_s
    {
        gpu::vec2i_t advance{};
        gpu::recti_s ink_bounds{};
        bool         has_ink{};
    };

    struct line_layout_s
    {
        line_metrics_s metrics;

        // UTF-32 code points to paint and to skip before the next line, respectively.
        // consumed_length includes a line separator or discarded wrap whitespace.
        size_t text_length{};
        size_t consumed_length{};
    };

  private:
    struct glyph_metrics_s
    {
        gpu::vec2i_t advance{};
        gpu::recti_s bounds{};
    };

    const std::shared_ptr<font_loader_s> loader_;
    std::vector<FT_Byte>                 file_data_;
    bool                                 valid_{};
    FT_Face                              face_{};

    int pixel_size_{};

    glyph_metrics_s load_glyph(FT_UInt index, bool render_bitmap);
    void            paint_glyph(surface_s& surface, gpu::vec2i_t position);
    line_layout_s
    layout_line(std::u32string_view str, std::optional<int> width, surface_s* surface, gpu::vec2i_t position);

  public:
    font_instance_s(std::shared_ptr<font_loader_s> loader, const std::filesystem::path& path, int index);
    font_instance_s(std::shared_ptr<font_loader_s> loader, std::vector<FT_Byte> data, int index);
    font_instance_s(const font_instance_s& other)            = delete;
    font_instance_s& operator=(const font_instance_s& other) = delete;
    font_instance_s(font_instance_s&& other)                 = delete;
    font_instance_s& operator=(font_instance_s&& other)      = delete;
    ~font_instance_s();

    void set_size(int size_in_px);

    bool valid() const { return valid_; }

    int line_height() const;

    // All operations stop at CR, LF or CRLF. Flow wraps at spaces/tabs; tabs are
    // one space. Width is measured by pen advance, with exact fits accepted.
    // A nonempty line always consumes at least one code point, even if overwide.
    // Glyphs use hinted metrics, signed integer pixels, and the same rounding
    // and color policy for measuring, wrapping and painting. Ink excludes spaces.
    line_layout_s measure_line(std::u32string_view str);
    line_layout_s flow_line(std::u32string_view str, int width);
    line_layout_s render_line(std::u32string_view str, surface_s* surface, gpu::vec2i_t baseline);
};

} // namespace miximus::render
