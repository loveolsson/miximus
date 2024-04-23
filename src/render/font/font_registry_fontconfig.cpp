#include "font_registry.hpp"
#include "logger/logger.hpp"

#include <fontconfig/fontconfig.h>

namespace miximus::render {

font_registry_s::font_registry_s()
{
    FcInit();
    auto config = FcInitLoadConfigAndFonts();
    auto pat    = FcPatternCreate();
    auto os     = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_LANG, FC_FILE, nullptr);
    auto fs     = FcFontList(config, pat, os);
    for (int i = 0; fs != nullptr && i < fs->nfont; ++i) {
        FcPattern* font   = fs->fonts[i];
        FcChar8*   file   = nullptr;
        FcChar8*   style  = nullptr;
        FcChar8*   family = nullptr;

        if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
            FcPatternGetString(font, FC_FAMILY, 0, &family) == FcResultMatch &&
            FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch) {
            const std::string family_str(reinterpret_cast<const char*>(family));
            const std::string style_str(reinterpret_cast<const char*>(style));
            std::string       path_str(reinterpret_cast<const char*>(file));

            font_variant_s v;
            v.index = 0;
            v.name  = style_str;
            v.path  = std::move(path_str);

            fonts_[family_str].variants.emplace(style_str, std::move(v));
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