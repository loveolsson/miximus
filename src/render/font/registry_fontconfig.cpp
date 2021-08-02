#include "logger/logger.hpp"
#include "registry.hpp"

#include <fontconfig/fontconfig.h>

namespace miximus::render::font {

font_registry_s::font_registry_s()
{
    auto log = getlog("app");
    log->debug("Scanning for system fonts");

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
            fonts_[reinterpret_cast<const char*>(family)].variants.emplace(
                reinterpret_cast<const char*>(style),
                font_variant_s{reinterpret_cast<const char*>(style), reinterpret_cast<const char*>(file)});
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