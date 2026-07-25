#ifndef _WIN32

#include "font_registry.hpp"
#include "logger/logger.hpp"
#include "utils/filesystem.hpp"

#include <fontconfig/fontconfig.h>
#include <string>
#include <string_view>
#include <utility>

namespace miximus::render {

font_registry_s::font_map_t font_registry_s::scan_fonts()
{
    font_map_t fonts;

    FcInit();
    auto config = FcInitLoadConfigAndFonts();
    auto pat    = FcPatternCreate();
    auto os     = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_LANG, FC_FILE, FC_INDEX, nullptr);
    auto fs     = FcFontList(config, pat, os);
    for (int i = 0; fs != nullptr && i < fs->nfont; ++i) {
        const FcPattern* font   = fs->fonts[i];
        FcChar8*         file   = nullptr; // NOLINT(misc-const-correctness)
        FcChar8*         style  = nullptr; // NOLINT(misc-const-correctness)
        FcChar8*         family = nullptr; // NOLINT(misc-const-correctness)

        if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
            FcPatternGetString(font, FC_FAMILY, 0, &family) == FcResultMatch &&
            FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch) {
            const std::string      family_str(reinterpret_cast<const char*>(family));
            const std::string      style_str(reinterpret_cast<const char*>(style));
            const std::string_view path_str(reinterpret_cast<const char*>(file));

            int fc_index = 0;
            FcPatternGetInteger(font, FC_INDEX, 0, &fc_index);

            font_variant_s v;
            v.index = fc_index;
            v.name  = style_str;
            v.path  = utils::path_from_utf8(path_str);

            auto& font_entry = fonts[family_str];
            font_entry.name  = family_str;
            font_entry.variants.emplace(style_str, std::move(v));
        }
    }

    if (fs != nullptr) {
        FcFontSetDestroy(fs);
    }

    if (os != nullptr) {
        FcObjectSetDestroy(os);
    }

    if (pat != nullptr) {
        FcPatternDestroy(pat);
    }

    if (config != nullptr) {
        FcConfigDestroy(config);
    }

    FcFini();

    return fonts;
}

} // namespace miximus::render

#endif // _WIN32
