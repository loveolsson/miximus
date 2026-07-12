#include "font_registry.hpp"
#include "logger/logger.hpp"

#include <fontconfig/fontconfig.h>
#include <string>
#include <utility>

namespace miximus::render {

void font_registry_s::scan_fonts()
{
    // Clear existing fonts
    fonts_.clear();

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
            const std::string family_str(reinterpret_cast<const char*>(family));
            const std::string style_str(reinterpret_cast<const char*>(style));
            std::string       path_str(reinterpret_cast<const char*>(file));

            int fc_index = 0;
            FcPatternGetInteger(font, FC_INDEX, 0, &fc_index);

            font_variant_s v;
            v.index = fc_index;
            v.name  = style_str;
            v.path  = std::move(path_str);

            auto& font_entry = fonts_[family_str];
            font_entry.name  = family_str;
            font_entry.variants.emplace(style_str, std::move(v));
        }
    }

    log_fonts();

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
}

} // namespace miximus::render
