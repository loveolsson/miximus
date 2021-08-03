#pragma once
#include "font_info.hpp"
#include "gpu/types.hpp"
#include "render/surface/surface_fwd.hpp"

#include <string_view>

// Fwd declarations
struct FT_LibraryRec_;
struct FT_FaceRec_;
typedef struct FT_LibraryRec_* FT_Library;
typedef struct FT_FaceRec_*    FT_Face;

namespace miximus::render {

class font_instance_s
{
    bool    valid_{};
    FT_Face face_{};

    friend class font_loader_s;

  public:
    font_instance_s(FT_Library library, const std::filesystem::path&, int index);
    ~font_instance_s();

    void set_size(int size_in_px);

    size_t fit_line(std::u32string_view str, int width);
    size_t draw_line(std::u32string_view str, surface_s* surface, gpu::vec2i_t pos);
};

} // namespace miximus::render
