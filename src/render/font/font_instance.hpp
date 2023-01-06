#pragma once
#include "font_info.hpp"
#include "gpu/types.hpp"
#include "render/surface/surface_fwd.hpp"

#include <ft2build.h>

#include FT_FREETYPE_H

#include <string_view>

namespace miximus::render {

class font_loader_s;

class font_instance_s
{
    const std::shared_ptr<font_loader_s> loader_;
    bool                                 valid_{};
    FT_Face                              face_{};

  public:
    struct flow_info_s
    {
        size_t consumed_chars;
        size_t pixels_advanced;
        bool   new_line;
    };

    font_instance_s(std::shared_ptr<font_loader_s> loader, const std::filesystem::path&, int index);
    ~font_instance_s();

    void set_size(int size_in_px);

    bool valid() const { return valid_; }

    flow_info_s  flow_line(std::u32string_view str, int width);
    gpu::vec2i_t render_string(std::u32string_view str, surface_s* surface, gpu::vec2i_t pos);
};

} // namespace miximus::render
