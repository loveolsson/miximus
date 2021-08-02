#include "logger/logger.hpp"
#include "registry.hpp"

#include <fontconfig/fontconfig.h>

namespace miximus::render::font {

font_registry_s::font_registry_s()
{
    FcInit();
    auto* config = FcInitLoadConfigAndFonts();
    auto* pat    = FcPatternCreate();
    auto* os     = FcObjectSetBuild(FC_FAMILY, FC_STYLE, FC_LANG, FC_FILE, (char*)0);
    auto* fs     = FcFontList(config, pat, os);
    for (int i = 0; fs && i < fs->nfont; ++i) {
        FcPattern* font = fs->fonts[i];
        FcChar8 *  file, *style, *family;

        if (FcPatternGetString(font, FC_FILE, 0, &file) == FcResultMatch &&
            FcPatternGetString(font, FC_FAMILY, 0, &family) == FcResultMatch &&
            FcPatternGetString(font, FC_STYLE, 0, &style) == FcResultMatch) {
            std::string family_str(reinterpret_cast<const char*>(family));
            std::string style_str(reinterpret_cast<const char*>(style));
            std::string path_str(reinterpret_cast<const char*>(file));

            font_variant_s v;
            v.index = 0;
            v.name  = style_str;
            v.path  = path_str;

            fonts_[family_str].variants.emplace(style_str, std::move(v));
        }
    }

    log_fonts();

    if (fs) {
        FcFontSetDestroy(fs);
    }

    if (os) {
        FcObjectSetDestroy(os);
    }

    if (pat) {
        FcPatternDestroy(pat);
    }

    if (config) {
        FcConfigDestroy(config);
    }

    FcFini();
}

} // namespace miximus::render::font